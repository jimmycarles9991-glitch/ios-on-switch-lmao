// SPDX-License-Identifier: MIT
// See gpu.h and command_buffer.h (CommandBuffer begin to commit).
#include "lock.h"
#include "event_release.h"
#include "gpu.h"
#include "TegraGM20BJobABI.h"
#include "slm.h"
#include "stamps.h"
#include "native_surface.h"
#include "tiled_cache.h"

#include <mach/mach.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// ---- command buffer ----

// Between command buffers: a ring that made an allocation wait for the GPU doubles (to `most`), the old one freed once the work
// that used it completed, so a client's rings follow what its command buffers use.
void Device::growRing(Ring &r, uint64_t most)
{
    const bool grow = r.waited && r.bo.bytes < most;
    r.waited = false;
    Bo larger;
    if (!grow || !k_.allocControl(std::min(r.bo.bytes * 2, most), larger, ControlUse::Ring))
        return;
    set_.add(k_, larger, &r == &upload_ ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead);
    grownOut_.push_back({r.bo, lastSubmitted(), channelGeneration()});
    r.bo = larger;
    r.head = r.since = r.clear = 0;
    r.inflight.clear();
}

void Device::reapAtBegin()
{
    if (!trimmedPending_.load(std::memory_order_acquire) && !nativeStale_.load(std::memory_order_acquire))
        return;
    DeferredFrees deferred(*this);
    std::vector<Backing *> trimmed;
    {
        SWITCH_METAL_ENCODE_LOCK(l, lock_);
        std::vector<Bo> nativeBos;
        reapNativeSurfaces(nativeBos);
        for (const Bo &bo : nativeBos)
            deferred.add({bo});
        nativeStale_.store(std::any_of(nativeSurfaces_.begin(), nativeSurfaces_.end(),
                                       [](const std::unique_ptr<NativeSurface> &s) {
                                           return !s->cached;
                                       }),
                           std::memory_order_relaxed);
        trimmed.swap(trimmedBackings_);
        trimmedPending_.store(false, std::memory_order_relaxed);
    }
    for (Backing *b : trimmed)
        delete b;
}

CommandBuffer *Device::begin()
{
    if (!ensureChannel())
        return nullptr;
    learnCompleted();
    Killed live = {};
    if ((contextless_ || k_.lost(channel_) || liveKill(live)) && !reopen())
        return nullptr;
    for (auto it = grownOut_.begin(); it != grownOut_.end();)
        if (it->unused(channelGeneration(), [this](uint32_t seqno) {
                return done(seqno);
            })) {
            k_.free(it->bo);
            it = grownOut_.erase(it);
        } else {
            ++it;
        }
    if (!movedPlain_.empty())
        freeMovedPlain();
    growRing(push_, kSizes[0].push);
    growRing(upload_, kSizes[0].upload);
#if SWITCH_METAL_DIAGNOSTICS
    if (glassMemOn.load(std::memory_order_relaxed) || glassMemLive_ || glassMemBusyCount_)
        glassMemFrame();
    cb_.glassMemSlot_ = -1;
#endif
    CommandBuffer &cb = cb_;
    cb.dev_ = this;
    cb.segments_.clear();
    cb.batchWork_ = {};
    cb.dirty_.clear();
    cb.backings_.clear();
    cb.heaps_.clear();
    cb.surfaces_.clear();
    cb.zcullPlanes_.clear();
    set_.view(setView_);
    cb.memberUses_.clear();
    cb.memberRemovals_ = setView_.removals;
    cb.memberHostWritten_ = false;
    cb.arenaKindInvalidate_ = false;
    cb.pendingKindSwitch_ = false;
    cb.kindSwitchGen_ = 0;
    cb.encodersOpened_ = 0;
    cb.activeBlit_ = nullptr;
    cb.inRender_ = false;
    cb.passBufferWrites_ = false;
    cb.prevBufferWrites_ = false;
    cb.tiledCacheDirty_ = false;
    cb.cbArenaKinds_.clear();
    cb.cbCrossSeen_.clear();
    cb.named_.clear();
    cb.sharedWaits_.clear();
    cb.sharedSignals_.clear();
    cb.sharedReleases_.clear();
    cb.predication_.endUnit();
    cb.loops_ = {};
    cb.visibility_ = JobVisibility::Legacy;
    cb.externalWrites_ = false;
    cb.volatileWrites_ = false;
    cb.publication_ = {};
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb.forceWholePrograms_ = cb.inlineStates_ = false;
    cb.splitProbe_ = SplitProbe::Twin;
    cb.splitForced_ = false;
    cb.splitCapturing_ = splitCaptureLeft.load(std::memory_order_relaxed) != 0;
    cb.splitCaptures_.clear();
    cb.foldCounts();
#endif
    cb.draws = cb.passes = cb.blits = cb.bands = 0;
    cb.refusal = {};
    cb.skipped = {};
    cb.skips = 0;
    cb.partial = false;
    cb.admissionBlocked = false;
    cb.quietEnd = false;
    cb.promises.clear();
    cb.promisesKept = false;
    cb.nativeError = 0;
    cb.kernelResult = 0;
    cb.notSubmitted = nullptr;
    cb.setRefused.clear();
    cb.nativeReads_.clear();
    cb.nativeSurfaces_.clear();
    cb.pitchImports_.clear();
    overflowing_ = false;
    overflow_.clear();
    cb.pushStart_ = push_.head;
    cb.uploadStart_ = upload_.head;
    memcpy(cb.rootPush_, rootPush_, sizeof rootPush_);
    cb.restart_ = restart_;
    cb.rtLayer_ = rtLayer_;
    cb.alphaHeld_ = alphaHeld_;
    cb.sampleMaskHeld_ = sampleMaskHeld_;
    cb.tiledCacheSize_ = tiledCacheSize_;
    cb.ds_ = ds_;
    cb.extrasLive_ = extrasLive_;
    cb.heldSegment_ = heldSegment_;
    cb.segmentBinds_ = 0;
    cb.drawn_ = false;
    reapAtBegin();
    // a trim's lock once in 64 command buffers, never waited for: the compressed-memory thread holding it trims too
    if (commits() % 64 == 0) {
        DeferredFrees deferred(*this);
        std::unique_lock<gm20b::Mutex> l(lock_, std::try_to_lock);
        if (l.owns_lock()) {
            SWITCH_METAL_ENCODE_COUNT(Locks);
            trimBackings(deferred);
        }
    }
    cb.stamp = stampOf(commits());
    cb.serial = ++begun_;
    SWITCH_METAL_DIAGNOSTIC(spans_.begin(cb.stamp));
    cb.timings.clear();
    cb.loopsLaid = false;
    // submission prologue, as NVK opens each submission: header/sampler caches (headers written since), constant, shader data and
    // texture caches, and the instruction caches where commit finds the heap grew
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, 64 * 4, 256, va));
    Push p{start, start + 64};
    emitStartStamp(p, stamps_.va, cb.stamp);
    cb.lastEngine_ = BarrierEngine::None;
    cb.chain_.reset();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb.forms_ = nextForms;
    nextForms = {};
    cb.taken_ = {};
    cb.pixelOpenings_ = 0;
#endif
    if (const EntryForm entry = entryFormFor(cb.barrierForms().entry); relaxed(entry)) {
        cb.shaderInvalidate_ = entryOpening(p, entry);
        SWITCH_METAL_DIAGNOSTIC(cb.taken_.entries++);
    } else {
        p.m(INVALIDATE_SAMPLER_CACHE_NO_WFI, 0);
        p.m(INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);
        p.inc(INVALIDATE_SHADER_CACHES_NO_WFI, {kInvalidateConstant});
        cb.shaderInvalidate_ = p.at - 1;
        p.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
        p.m(INVALIDATE_SHADER_CACHES_NO_WFI, kInvalidateData);
    }
    SWITCH_METAL_DIAGNOSTIC(beginDrawTimes(cb, p));
    checkRoom(p, "a submission prologue");
    cb.segment(start, p.at);
    cb.prologueDwords_ = uint32_t(p.at - start);
    cb.splits_.clear();
    cb.splitRecords_.clear();
    cb.splitReuse_.clear();
    cb.bufferWrites_.clear();
    cb.geometryBudget_ = {};
    cb.geometryPool_.clear();
    return &cb;
}

LoopStream CommandBuffer::loopStream(Push &p, uint32_t *&start)
{
    return {p, start, segments_, dev_->push_.bo.cpu, dev_->push_.bo.va};
}

void CommandBuffer::segment(uint32_t *from, uint32_t *to)
{
    if (to == from)
        return;
    const uint64_t va = dev_->push_.bo.va + (reinterpret_cast<uint8_t *>(from) - dev_->push_.bo.cpu);
    segments_.append({va, uint32_t(to - from), 0});
}

void CommandBuffer::timestamps(const counters::Stamp *stamps, size_t count)
{
    if (!count)
        return;
    const uint32_t words = uint32_t(count) * counters::kTimestampWords;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, words * 4, 256, va));
    Push p{start, start + words};
    for (size_t i = 0; i < count; i++) {
        useAddress(stamps[i].va, counters::kRecordBytes, true);
        counters::emit(p, stamps[i]);
    }
    checkRoom(p, "counter samples");
    segment(start, p.at);
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p.at) - dev_->push_.bo.cpu);
}

void CommandBuffer::releaseEvent(uint64_t va, uint32_t payload)
{
    uint64_t pushVa;
    auto *words = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, kEventReleaseWords * 4, 4, pushVa));
    Push p{words, words + kEventReleaseWords};
    // a kernel without addressed jobs publishes DRAM at every job end, so its mid-job releases do as well
    const bool publish = publication_.take() || !dev_->k_.exactJobs();
    emitEventRelease(p, va, payload, publish);
    segment(words, p.at);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (!publish)
        dev_->unflushed_.fetch_add(1, std::memory_order_relaxed);
#endif
}

uint64_t CommandBuffer::transient(uint32_t bytes)
{
    uint64_t va = 0;
    dev_->alloc(dev_->upload_, std::max(bytes, 16u), 256, va);
    return va;
}

std::shared_ptr<ResourceReadyCompletion> CommandBuffer::resourceReadiness(std::function<void()> ready, int &result)
{
    auto events = dev_->k_.sharedEvents();
    result = kIOReturnUnsupported;
    return events ? events->resources(surfaces_.uses(), std::move(ready), result) : nullptr;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void CommandBuffer::foldCounts()
{
    Device::Stats &stats = dev_->stats;
    stats.uploadBytes += counts_.uploadBytes;
    stats.segmentBinds += counts_.segmentBinds;
    stats.segmentWords += counts_.segmentWords;
    stats.segmentSkips += counts_.segmentSkips;
    stats.segmentCopies += counts_.segmentCopies;
    stats.blendKillBinds += counts_.blendKillBinds;
    stats.interlockDraws += counts_.interlockDraws;
    stats.rasterOrderedDraws += counts_.rasterOrderedDraws;
    stats.fetch.interlocked += counts_.interlocked;
    stats.fetch.interlockedBlends += counts_.interlockedBlends;
    stats.fetch.twinRule += counts_.twinRule;
    stats.splitTwins += counts_.splitTwins;
    for (size_t k = 0; k < size_t(SplitWhole::Count); k++)
        stats.splitWhole[k] += counts_.splitWhole[k];
    counts_ = {};
}
#endif

void CommandBuffer::describeRefusal(char *line, size_t size) const
{
    if (!setRefused.empty())
        snprintf(line, size, "%s", setRefused.c_str());
    else if (kernelResult)
        snprintf(line, size, "the kernel refused its job (0x%x)", unsigned(kernelResult));
    else
        snprintf(line, size, "%s", notSubmitted ? notSubmitted : "nothing named why");
}

// One error line a commit that submits nothing more for a reason its caller does not take up again (the CPU holding a
// use, a native surface changed under it): the first 16, then every 256th
static void logNotSubmitted(const CommandBuffer &cb)
{
    if (cb.kernelResult == kIOReturnCannotLock || (cb.nativeError && cb.kernelResult == cb.nativeError))
        return;
    static std::atomic<uint32_t> lines{0};
    const uint32_t n = lines.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 16 && n % 256)
        return;
    char why[192];
    cb.describeRefusal(why, sizeof why);
    os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
                 "SwitchMetal: a command buffer not submitted: %{public}s (#%u)", why, n);
}

bool CommandBuffer::commit(uint32_t &seqno)
{
    struct NotSubmittedLine {
        const CommandBuffer &cb;
        bool submitted = false;

        ~NotSubmittedLine()
        {
            if (!submitted && (cb.kernelResult || cb.notSubmitted || !cb.setRefused.empty()))
                logNotSubmitted(cb);
        }
    } notSubmittedLine{*this};

    SWITCH_METAL_ENCODE_SPAN(Commit);
    SWITCH_METAL_ENCODE_COUNT(GeometryVertices, GeometryBudget().vertices - geometryBudget_.vertices);
    if (nativeError)
        return false;

    Device &d = *dev_;
    SWITCH_METAL_DIAGNOSTIC(foldCounts());
    // a commit that fails after some of its jobs ran leaves the channel holding a segment the device cannot name
    d.heldSegment_.lose();
    launchPrologues();
    if (d.overflowing_) {
        notSubmitted = "a command buffer larger than its ring, with no memory for a larger one";
        return false;
    }
    bool invalidates = false, published = false;
    uint64_t invalidatedBefore = 0;
    bool tablesInvalidate = false;
    uint64_t tablesBefore = 0;

    struct RollbackInvalidation {
        Device &device;
        bool &invalidates, &published;
        uint64_t &before;
        bool &tables;
        uint64_t &tablesBefore;

        ~RollbackInvalidation()
        {
            if (invalidates && !published)
                device.heapInvalidated_ = before;
            if (tables && !published)
                device.tablesInvalidated_ = tablesBefore;
        }
    } rollback{d, invalidates, published, invalidatedBefore, tablesInvalidate, tablesBefore};

    uint64_t slmGiven = 0, slmVa = 0;
    const uint64_t placed = d.heapEpoch_.load(std::memory_order_acquire);
    if (shaderInvalidate_ && placed != d.heapInvalidated_) {
        *shaderInvalidate_ |= kInvalidateInstruction;
        invalidates = true;
        invalidatedBefore = d.heapInvalidated_;
        d.heapInvalidated_ = placed;
    }
    if (const uint64_t perWarp = d.slmPerWarp_.load(std::memory_order_acquire); perWarp != d.slmChannel_) {
        slmGiven = perWarp;
        slmVa = d.slmVa_.load(std::memory_order_relaxed);
    }
    // ahead of everything in it: a program placed after begin() may ask for more than the channel has
    if (slmGiven && !segments_.empty()) {
        uint64_t va;
        auto *head = reinterpret_cast<uint32_t *>(d.alloc(d.push_, kSlmWords * 4, 4, va));
        Push p{head, head + kSlmWords};
        emitSlm(p, slmVa, slmArea(slmGiven, d.k_.info().tpc_count));
        segments_.insert(0, {va, uint32_t(p.at - head), 0});
    }
#if SWITCH_METAL_DIAGNOSTICS
    if (!segments_.empty() && d.glassMemLive_)
        glassMemTail();
#endif
    if (!segments_.empty()) {
        uint64_t va;
        auto *tail = reinterpret_cast<uint32_t *>(d.alloc(d.push_, (kTiledCacheFlushWords + kStampWords) * 4, 4, va));
        Push p{tail, tail + kTiledCacheFlushWords + kStampWords};
        if (tiledCache(d.k_.info()) && tiledCacheDirty_)
            p.at = tiledCacheFlush(p.at);
        emitEndStamp(p, d.stamps_.va, stamp);
        segment(tail, p.at);
    }
    for (const auto &range : dirty_.merged()) {
#if defined(__aarch64__)
        if (range.invalidate) {
            for (uintptr_t address = range.begin; address < range.end; address += 64)
                __asm__ volatile("dc civac, %0" ::"r"(address) : "memory");
        } else {
            for (uintptr_t address = range.begin; address < range.end; address += 64)
                __asm__ volatile("dc cvac, %0" ::"r"(address) : "memory");
        }
#endif
        SWITCH_METAL_DIAGNOSTIC(d.stats.cleanBytes += range.end - range.begin);
    }
#if defined(__aarch64__)
    // The cleans and WC ring writes complete before the kernel rings GP_PUT.
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    // A loop's copies end with the GPU's writes into the push ring (loop_copies.h): the ring is a member the set
    // otherwise names read-only
    if (loopsLaid) {
        useAddress(d.push_.bo.va, d.push_.bo.bytes, true);
        for (const Bo &bo : d.push_.outgrown)
            useAddress(bo.va, bo.bytes, true);
    }
    if (d.k_.exactJobs()) {
        // the handle tables and heap sets the CPU wrote since the last submission that invalidated the L2 for them, set
        // members the job's flag names host-written
        const uint64_t written = d.tablesWritten_.load(std::memory_order_acquire);
        if (written != d.tablesInvalidated_) {
            tablesInvalidate = true;
            tablesBefore = d.tablesInvalidated_;
            d.tablesInvalidated_ = written;
        }
        if (!heaps_.empty()) {
            std::unique_lock<gm20b::Mutex> lock(d.lock_);
            SWITCH_METAL_ENCODE_COUNT(Locks);
            nameHeaps(lock);
        }
    }
    if (!batchWork_.operations)
        segments_.includeTail();
    const uint32_t maxPushes = d.k_.maxPushes(), maxFences = d.k_.maxFences();
    if (!segments_.valid(maxPushes)) {
        notSubmitted = "a continuation group past the pushes one job takes";
        return false;  // validate all indivisible continuation groups before publishing any prefix
    }
    // A command buffer of several jobs holds its shared uses' CPU-lock admission from its first job to its last
    const bool multiple = (!segments_.empty() && segments_.submission(0, maxPushes, maxFences) < segments_.size()) ||
                          sharedWaits_.size() + sharedSignals_.size() > TegraGM20BMaxJobEvents;
    const uint64_t admission = multiple ? d.k_.nextAdmission() : 0;
    bool admissionHeld = false;
    uint32_t seq = 0;
    auto submit = [&](const TegraGM20BAbiPush *pushes, uint32_t count, SurfaceList &resources, JobVisibility visibility,
                      const JobEvents &events, bool last) {
        int result = KERN_SUCCESS;
        SurfaceList &uses = admission ? surfaces_ : resources;
        const uint32_t admissionFlags =
            admission ? (published ? uint32_t(TegraGM20BJobAdmissionContinue) : uint32_t(TegraGM20BJobAdmissionBegin)) |
                            (last ? uint32_t(TegraGM20BJobAdmissionEnd) : 0u)
                      : 0u;
        SWITCH_METAL_DIAGNOSTIC(noteExecUses(uses.uses().size()));
        SWITCH_METAL_DIAGNOSTIC(noteExecPushes(count));
        SWITCH_METAL_ENCODE_BEGIN(execSpan, Exec);
        const bool executed =
            d.job(d.setView_, d.channel_, pushes, count, uses, visibility, seq, &result, events, &memberUses_,
                  memberRemovals_, invalidates || memberHostWritten_ || tablesInvalidate || arenaKindInvalidate_,
                  admission, admissionFlags, !last || quietEnd, last && !promises.empty() ? &promises : nullptr);
        SWITCH_METAL_ENCODE_END(execSpan);
        if (executed && last && quietEnd && gm20b::jobKeptLoud())
            quietEnd = false;
        if (!executed) {
            if (!uses.valid())
                notSubmitted = "a job's uses past what one job names, or a use of no backing";
            else
                kernelResult = result;
            admissionBlocked = !published && result == kIOReturnCannotLock;
            setRefused = d.setView_.refused;
            if (admissionHeld) {
                SurfaceList none;
                uint32_t cancelled = 0;
                (void)d.job(d.setView_, d.channel_, nullptr, 0, none, JobVisibility::Internal, cancelled, nullptr, {},
                            nullptr, 0, false, admission, TegraGM20BJobAdmissionCancel);
                admissionHeld = false;
            }
            if ((!nativeSurfaces_.empty() || !sharedWaits_.empty() || !sharedSignals_.empty()) &&
                (result == kIOReturnNotReady || result == kIOReturnAborted))
                nativeError = result;
            if (published) {
                partial = true;
                d.wait(d.lastSubmitted());
                d.contextless_ = true;
            }
            return false;
        }
        published = true;
        promisesKept |= last && !promises.empty();
        admissionHeld = admission && !last;
        d.submittedThrough(seq);
        return true;
    };
    SurfaceList eventResources;
    size_t waited = 0, signaled = 0;
    // Capacity is an ABI bound, not permission to truncate dependencies. Prefix joins use the same channel's order.
    while (sharedWaits_.size() - waited + std::min<size_t>(sharedSignals_.size(), TegraGM20BMaxJobEvents) >
           TegraGM20BMaxJobEvents) {
        const uint32_t count = uint32_t(std::min<size_t>(sharedWaits_.size() - waited, TegraGM20BMaxJobEvents));
        const JobEvents events{sharedWaits_.data() + waited, nullptr, count, 0};
        if (!submit(nullptr, 0, eventResources, JobVisibility::Internal, events, false))
            return false;
        waited += count;
    }
    const bool pushed = segments_.submit(maxPushes, maxFences, [&](size_t at, size_t n) {
        const bool closing = at + n == segments_.size();
        const JobVisibility volatileVis = d.k_.volatileWrites() ? JobVisibility::Volatile : JobVisibility::External;
        const JobVisibility visibility = visibility_ == JobVisibility::Legacy ? JobVisibility::Legacy
                                         : externalWrites_ && closing         ? JobVisibility::External
                                         : volatileWrites_ && closing         ? volatileVis
                                                                              : JobVisibility::Internal;
        const uint32_t waitCount = uint32_t(sharedWaits_.size() - waited);
        const uint32_t signalCount =
            closing ? uint32_t(std::min<size_t>(sharedSignals_.size(), TegraGM20BMaxJobEvents - waitCount)) : 0;
        const JobEvents events{waitCount ? sharedWaits_.data() + waited : nullptr,
                               signalCount ? sharedSignals_.data() : nullptr, waitCount, signalCount,
                               signalCount ? sharedReleases_.data() : nullptr};
        const bool last =
            closing && waited + waitCount == sharedWaits_.size() && signaled + signalCount == sharedSignals_.size();
        if (!submit(segments_.data() + at, uint32_t(n), surfaces_, visibility, events, last))
            return false;
        waited += waitCount;
        signaled += signalCount;
        for (NativeSurface *surface : nativeSurfaces_) {
            surface->lastUse = seq;
            surface->channelGeneration = d.channelGeneration();
        }
        return true;
    });
    if (!pushed)
        return false;
    while (waited < sharedWaits_.size() || signaled < sharedSignals_.size()) {
        const uint32_t waitCount = uint32_t(std::min<size_t>(sharedWaits_.size() - waited, TegraGM20BMaxJobEvents));
        const uint32_t signalCount =
            uint32_t(std::min<size_t>(sharedSignals_.size() - signaled, TegraGM20BMaxJobEvents - waitCount));
        const JobEvents events{waitCount ? sharedWaits_.data() + waited : nullptr,
                               signalCount ? sharedSignals_.data() + signaled : nullptr, waitCount, signalCount,
                               signalCount ? sharedReleases_.data() + signaled : nullptr};
        const bool last = waited + waitCount == sharedWaits_.size() && signaled + signalCount == sharedSignals_.size();
        if (!submit(nullptr, 0, eventResources, JobVisibility::External, events, last))
            return false;
        waited += waitCount;
        signaled += signalCount;
    }
    seqno = d.lastSubmitted();
#if SWITCH_METAL_DIAGNOSTICS
    if (glassMemSlot_ >= 0) {
        d.glassMemCbs_[glassMemSlot_].seqno = seqno;
        d.glassMemBusy_[glassMemSlot_] = true;
        d.glassMemBusyCount_++;
        glassMemSlot_ = -1;
    }
#endif
    if (slmGiven)
        d.slmChannel_ = slmGiven;
    d.retire(d.push_, seq);
    d.retire(d.upload_, seq);
    memcpy(d.rootPush_, rootPush_, sizeof rootPush_);
    d.restart_ = restart_;
    d.rtLayer_ = rtLayer_;
    d.alphaHeld_ = alphaHeld_;
    d.sampleMaskHeld_ = sampleMaskHeld_;
    d.tiledCacheSize_ = tiledCacheSize_;
    d.ds_ = ds_;
    d.extrasLive_ = extrasLive_;
    d.heldSegment_ = heldSegment_;
    const uint64_t now = compClock();
    for (Texture *b : backings_) {
        Device::Backing &k = Device::backingOf(*b);
        k.lastUse.store(seq, std::memory_order_relaxed);
        k.submitted.store(true, std::memory_order_relaxed);
        k.usedNs.store(now, std::memory_order_relaxed);
    }
    d.commitCount_.store(d.commitCount_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    d.closedCount_.store(d.closedCount_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    for (const auto &[id, gen] : cbCrossSeen_) {
        uint64_t &seen = d.arenaCrossSeen_[id];
        if (gen > seen)
            seen = gen;
    }
    cbCrossSeen_.clear();
    if (invalidates || memberHostWritten_ || tablesInvalidate || arenaKindInvalidate_ || kindSwitchGen_) {
        d.l2InvalidatedBeforeCb_ = serial;
        for (auto it = d.arenaKindUses_.begin(); it != d.arenaKindUses_.end();) {
            it->second.pruneBefore(d.l2InvalidatedBeforeCb_);
            it = it->second.empty() ? d.arenaKindUses_.erase(it) : std::next(it);
        }
    }
    for (const auto &entry : cbArenaKinds_) {
        const uint64_t id = entry.first;
        entry.second.each([&](uint64_t offset, uint64_t bytes, uint32_t kind, uint64_t gen) {
            if (gen == kindSwitchGen_)
                d.arenaKindUses_[id].record(offset, bytes, kind, serial);
        });
    }
    cbArenaKinds_.clear();
    d.noteCommittedTimings(timings);
    SWITCH_METAL_DIAGNOSTIC(if (drawTimes_) d.committedDrawTimes(*this, seq));
    releaseNativeReads();
    SWITCH_METAL_DIAGNOSTIC(d.stats.commits++);
    SWITCH_METAL_DIAGNOSTIC(d.stats.passPixelOpenings.fetch_add(pixelOpenings_, std::memory_order_relaxed));
    notSubmittedLine.submitted = true;
    return true;
}

void CommandBuffer::abandon()
{
    releaseNativeReads();
    Device &d = *dev_;
    d.closedCount_.store(d.closedCount_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    d.overflowing_ = false;
    d.overflow_.clear();
    for (Device::Ring *r : {&d.push_, &d.upload_}) {
        // drop the open command buffer's spans and give back what it took (nothing else was allocated since: one encodes)
        for (auto it = r->inflight.begin(); it != r->inflight.end();)
            it = it->submitted ? std::next(it) : r->inflight.erase(it);
        r->head = r->since = (r == &d.push_ ? pushStart_ : uploadStart_);
        if (!r->outgrown.empty()) {  // it began in a ring it grew out of: the new one is empty
            for (const Bo &b : r->outgrown)
                d.grownOut_.push_back({b, d.lastSubmitted(), d.channelGeneration()});
            r->outgrown.clear();
            r->head = r->since = 0;
        }
    }
    segments_.clear();
    dirty_.clear();
    surfaces_.clear();
    named_.clear();
    cbArenaKinds_.clear();
    cbCrossSeen_.clear();
    activeBlit_ = nullptr;
    inRender_ = false;
    passBufferWrites_ = false;
    prevBufferWrites_ = false;
    tiledCacheDirty_ = false;
    pendingKindSwitch_ = false;
    arenaKindInvalidate_ = false;
}

}  // namespace gm20b
