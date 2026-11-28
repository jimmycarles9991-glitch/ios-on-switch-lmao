// SPDX-License-Identifier: MIT
// See gpu.h (the channel's fences, kills and reopen).
#include "lock.h"
#include "gpu.h"
#include "TegraGM20BLayout.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// A wait the kernel refuses with no kill (the call failed, or the submit thread's reopen destroyed the channel meanwhile) is
// no completion: the channel's status decides. A healthy channel is waited on again; one that keeps refusing is taken as
// killed, with everything it has not completed, so nothing it may still run is released or written over as done.
bool Device::waitFence(uint32_t seqno, uint64_t generation)
{
    constexpr unsigned kRefusals = 3;
    const uint32_t ch = channel_.load(std::memory_order_acquire), opened = k_.opened();
    if (done(seqno) && semanticFence(seqno).status == SemanticFenceStatus::Superseded) {
        int result = KERN_SUCCESS;
        if (k_.wait(ch, seqno, ~0ull, &result))
            return !lost(seqno);
        if (result == kIOReturnAborted) {
            keepFailure(seqno);
            return false;
        }
    }
    for (unsigned refused = 0; !done(seqno);) {
        if (channelGeneration() != generation)
            return false;
        if (k_.wait(ch, seqno))
            break;
        if (channelGeneration() != generation)
            return false;
        TegraGM20BAbiStatus st;
        if (k_.lost(ch) || !k_.status(ch, st)) {  // killed, or destroyed by a reopen that kept its kill record
            break;
        }
        if (st.error) {
            k_.markLost(ch, opened);
            break;
        }
        if (done(seqno))
            return !lost(seqno);
        if (++refused == kRefusals) {
            os_log_error(gm20b::log(),
                         "SwitchMetal: the kernel refused %u waits on channel %u's seqno %u (%s): taken as killed",
                         refused, ch, seqno, k_.error().c_str());
            {
                std::lock_guard<gm20b::UnfairMutex> l(killedLock_);
                killedKept_[killedNext_++ % killedKept_.size()] = {st.completed + 1, lastSubmitted(), kWaitRefused};
                keptThrough_.store(lastSubmitted(), std::memory_order_relaxed);
                keptKills_.store(true, std::memory_order_release);
            }
            k_.markLost(ch, opened);  // begin() makes it again
            return false;
        }
    }
    return !lost(seqno);
}

// A job the kernel says its own event wait failed, which the fence record no longer names
void Device::keepFailure(uint32_t seqno)
{
    std::lock_guard<gm20b::UnfairMutex> l(killedLock_);
    killedKept_[killedNext_++ % killedKept_.size()] = {seqno, seqno, kDependencyFailed};
    const uint32_t through = keptThrough_.load(std::memory_order_relaxed);
    if (!keptKills_.load(std::memory_order_relaxed) || int32_t(seqno - through) > 0)
        keptThrough_.store(seqno, std::memory_order_relaxed);
    keptKills_.store(true, std::memory_order_release);
}

// What the live channel failed, for its retirement: the kept records (its kill record among them) and the newest job its
// fence record names failed
std::vector<KillRange> Device::failedSeqnos() const
{
    std::vector<KillRange> failed;
    {
        std::lock_guard<gm20b::UnfairMutex> l(killedLock_);
        for (const Killed &kept : killedKept_)
            if (kept.error)
                failed.push_back(kept);
    }
    const uint32_t ch = channel_.load(std::memory_order_acquire);
    const volatile uint32_t *fence = fence_.load(std::memory_order_acquire);
    Killed live = {};
    if (!k_.killRecords())  // nothing tells the seqnos the channel finished from those it took with it
        failed.push_back({1, lastSubmitted(), kWaitRefused});
    else if (k_.lost(ch) && !liveKill(live))  // the kernel publishes the error before the record and the fence
        failed.push_back({__atomic_load_n(fence, __ATOMIC_ACQUIRE) + 1, lastSubmitted(),
                          k_.lostError() ? k_.lostError() : kWaitRefused});
    const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence);
    if (const uint32_t seqno =
            newestSemanticFailure(reinterpret_cast<const volatile TegraGM20BAbiFenceSemantic *>(record + 1)))
        failed.push_back({seqno, seqno, kDependencyFailed});
    return failed;
}

// A kept record names the seqnos of the slot it was written in, which a channel made in another slot gives out again
void Device::forgetKept()
{
    std::lock_guard<gm20b::UnfairMutex> l(killedLock_);
    killedKept_ = {};
    keptThrough_.store(0, std::memory_order_relaxed);
    keptKills_.store(false, std::memory_order_release);
}

// The barriers order the record after the caller's read of the fence word, and its range after its error: the kernel
// writes the range, the error, then the fence word.
bool Device::liveKill(Killed &k) const
{
    const volatile uint32_t *fence = fence_.load(std::memory_order_acquire);
    if (fence == &kNoFence)
        return false;
    if (k_.killRecords()) {
        const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence);
        std::atomic_thread_fence(std::memory_order_acquire);
        k.error = record->error;
        if (k.error) {
            std::atomic_thread_fence(std::memory_order_acquire);
            k.first = record->killed_first;
            k.last = record->killed_last;
            return true;
        }
    }
    return false;
}

bool Device::killed(uint32_t seqno) const
{
    const auto covers = [seqno](const Killed &k) {
        return k.error && int32_t(seqno - k.first) >= 0 && int32_t(k.last - seqno) >= 0;
    };
    Killed live = {};
    if (liveKill(live) && covers(live)) {
        killedError_.store(live.error, std::memory_order_relaxed);
        return true;
    }
    if (semanticFence(seqno).status == SemanticFenceStatus::Failed) {
        killedError_.store(kDependencyFailed, std::memory_order_relaxed);
        return true;
    }
    if (!keptKills_.load(std::memory_order_acquire) ||
        int32_t(seqno - keptThrough_.load(std::memory_order_relaxed)) > 0)
        return false;
    std::lock_guard<gm20b::UnfairMutex> l(killedLock_);
    for (const Killed &kept : killedKept_)
        if (covers(kept)) {
            killedError_.store(kept.error, std::memory_order_relaxed);
            return true;
        }
    return false;
}

// The kernel killed the channel (a GPU fault or hang, its own or one it was stuck behind; as a GPU restart on iOS): what it
// had in flight failed, and a new channel, named by ChannelCreate's reply, gets the context the old one took with it.
// Between command buffers: no span of either ring is open, and every one the old channel had is done (the kernel released
// its fence), whatever word the new channel's fence is.
std::string Device::vaOwner(uint64_t va)
{
    char line[96];
    auto in = [&](const Bo &bo, uint64_t bytes, const char *what) {
        const uint64_t base = bo.vaBase ? bo.vaBase : bo.va;
        if (!base || va < base || va - base >= bytes)
            return false;
        snprintf(line, sizeof line, "%s +0x%llx", what, (unsigned long long)(va - base));
        return true;
    };
    auto chunk = [&](const std::vector<Chunk> &chunks, const char *what) {
        for (size_t i = 0; i < chunks.size(); i++) {
            if (chunks[i].bo.handle && in(chunks[i].bo, chunks[i].bo.bytes, what))
                return true;
            if (const uint64_t view = chunks[i].pitchVa; view && va >= view && va - view < chunks[i].bytes) {
                snprintf(line, sizeof line, "%s's kind-0 view +0x%llx", what, (unsigned long long)(va - view));
                return true;
            }
        }
        return false;
    };
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (in(heap_, heapBound_, "the shader heap") || in(heap_, heap_.bytes, "the shader heap's unbound reservation") ||
        in(push_.bo, push_.bo.bytes, "the push ring") || in(upload_.bo, upload_.bo.bytes, "the upload ring") ||
        in(root_, root_.bytes, "the root table") || in(tic_, tic_.bytes, "the texture headers") ||
        in(tsc_, tsc_.bytes, "the sampler headers") || in(zero_, zero_.bytes, "the zero page") ||
        in(tickets_, tickets_.bytes, "the interlock tickets") ||
        in(colorTable_, colorTable_.bytes, "the colour table") ||
        in(srgbEncode_, srgbEncode_.bytes, "the sRGB encode table") ||
        in(stamps_, stamps_.bytes, "the GPU time stamps") ||
        in(handleTables_, handleTables_.bytes, "the argument buffers' handle tables") ||
        in(argumentRoots_, argumentRoots_.bytes, "the argument buffers' root table") ||
        std::any_of(heapSets_.begin(), heapSets_.end(),
                    [&](const Bo &bo) {
                        return in(bo, bo.bytes, "an argument buffers' heap set");
                    }) ||
        chunk(chunks_, "a pool chunk") || chunk(blChunks_, "a block-linear chunk") ||
        std::any_of(std::begin(zChunks_), std::end(zChunks_),
                    [&](const std::vector<Chunk> &z) {
                        return chunk(z, "a zeta chunk");
                    }) ||
        chunk(wcChunks_, "a write-combined chunk") || chunk(volatileWcChunks_, "a volatile write-combined chunk"))
        return line;
    for (const auto &[key, i] : surfaces_)
        if (in(i->bo, i->bo.bytes, i->kind == TGM_LAYOUT_KIND ? "a block-linear IOSurface" : "a pitch IOSurface"))
            return std::string(line) + " (IOSurface " + std::to_string(i->id) + ")";
    return "none of the device's memory";
}

bool Device::reopen()
{
#if SWITCH_METAL_DIAGNOSTICS
    const uint64_t began = mach_absolute_time();
#endif
    if (const uint32_t oldChannel = channel_.load(std::memory_order_relaxed)) {
        // none after a reopen the kernel refused: that channel was retired then
        keptSlot_ = fence_.load(std::memory_order_relaxed);
        Killed live = {};
        if (liveKill(live)) {
            k_.markLost(oldChannel, k_.opened());
            std::lock_guard<gm20b::UnfairMutex> l(killedLock_);
            killedKept_[killedNext_++ % killedKept_.size()] = live;
            keptThrough_.store(live.last, std::memory_order_relaxed);
            keptKills_.store(true, std::memory_order_release);
        }
        contextless_ = true;
        const uint64_t retiring = generations_.retire(failedSeqnos());
        // Closed once: the connection is NVK's, whose next channel may take the slot number
        k_.closeChannel(oldChannel);
        generations_.closed(retiring);
        channel_.store(0, std::memory_order_release);
        fence_.store(&kNoFence, std::memory_order_release);
        setView_.useCache.clear();
        progressVa_ = 0;
        progressSeq_ = 0;
    }
    if (!channelRefusal_.mayAsk(k_))
        return false;
    bool gaveBack = false;
    if (!makeChannel(k_, kEngines, channel_, fence_, lastSeqno_, &kNoFence,
                     [this, &gaveBack](uint32_t channel, uint32_t &seqno) {
                         gaveBack = !newContext(channel, seqno);
                         return !gaveBack;
                     })) {
        channelRefusal_.refused(gaveBack);
        std::lock_guard<gm20b::UnfairMutex> l(channelLock_);
        channelError_ = k_.error();
        return false;
    }
    channelRefusal_.made();
    progressVa_ = 0;
    progressSeq_ = 0;
    k_.channelProgress(channel_.load(std::memory_order_relaxed), progressVa_);
    if (fence_.load(std::memory_order_relaxed) != keptSlot_)
        forgetKept();
    contextless_ = false;
    SWITCH_METAL_DIAGNOSTIC(stats.reopenTicks = mach_absolute_time() - began);
    return true;
}

// A channel made again: the context the dead one took with it, before anything else runs on it
bool Device::newContext(uint32_t channel, uint32_t &seq)
{
    stats.reopens++;
    push_.inflight.clear();
    upload_.inflight.clear();
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        for (Backing *b : backings_)
            b->submitted = false;
        heapInvalidated_ = ~0ull;
        ticInvalidated_ = ~0ull;
        tscInvalidated_ = ~0ull;
    }
    slmChannel_ = 0;
    memset(rootPush_, 0, sizeof rootPush_);
    restart_ = 0;
    rtLayer_ = ~0u;
    alphaHeld_ = kAlphaControlUnknown;
    sampleMaskHeld_ = kSampleMaskUnknown;
    clampRangeHeld_ = false;
    const uint32_t words = contextWords() + uint32_t(fx::twodInitWords());
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, words * 4, 256, va));
    Push p{start, start + words};
    emitContext(p);
    // The interlock's tickets start at zero with the new context's dispensers (a fresh context: its dispenser values are
    // zero already). Warps of the dead channel may have held tickets, and their lines may sit dirty in the GPU's L2, so the
    // copy engine zeroes the buffer in the new channel's own order, done before the 3D engine runs again.
    // in copies of the zero page, which is smaller than the buffer
    for (uint64_t done = 0; done < tickets_.bytes; done += zero_.bytes) {
        const uint64_t to = tickets_.va + done;
        p.inc(CE_OFFSET_IN_UPPER, {uint32_t(zero_.va >> 32), uint32_t(zero_.va), uint32_t(to >> 32), uint32_t(to)},
              SUBC_COPY);
        p.inc(CE_LINE_LENGTH_IN, {uint32_t(std::min<uint64_t>(zero_.bytes, tickets_.bytes - done)), 1}, SUBC_COPY);
        p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);
    }
    p.m(WAIT_FOR_IDLE, 0);
    fx::emitTwodInit(p);
    TegraGM20BAbiPush seg = {va, uint32_t(p.at - start), 0};
    SurfaceList resources;
    if (!member(zero_))
        resources.add(zero_.handle, false);
    if (!member(tickets_))
        resources.add(tickets_.handle, true);
    if (!job(setView_, channel, &seg, 1, resources, JobVisibility::External, seq))
        return false;
    retire(push_, seq);
    return true;
}

}  // namespace gm20b
