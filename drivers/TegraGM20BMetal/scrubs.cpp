// SPDX-License-Identifier: MIT
// Zero-on-free of Shared buffers' memory (gpu.h Scrub): a freed span of a VOL=1 write-combined chunk stays used until the
// copy engine has written zeros over it, so the create that takes it again does no CPU work, and a chunk that empties
// parks or is kept clean (one that goes back to the kernel needs no zeroing). Metal clears every buffer
// newBufferWithLength: makes, in every storage mode.
#include "lock.h"
#include "gpu.h"
#include "blit_encoder.h"

#include <algorithm>
#include <iterator>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// lock_ held: back in its chunk's free map, the chunk given back as an emptied write-combined one is (wcPoolFree)
void Device::scrubbedFree(const Scrub &s, DeferredFrees *deferred)
{
    std::vector<Chunk> &pool = volatileWcChunks_;
    if (s.chunk >= pool.size())
        return;
    Chunk &ch = pool[s.chunk];
    if (ch.park != ChunkPark::Live || ch.bo.va + s.offset != s.va)
        return;
    ch.used -= s.bytes;
    uint64_t offset = s.offset, bytes = s.bytes;
    auto next = ch.free.lower_bound(offset);
    if (next != ch.free.end() && offset + bytes == next->first) {
        bytes += next->second;
        next = ch.free.erase(next);
    }
    bool merged = false;
    if (next != ch.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += bytes;
            merged = true;
        }
    }
    if (!merged)
        ch.free[offset] = bytes;
    if (ch.used || !deferred || !otherEmptyChunk(pool, ch))
        return;
    deferred->add({ch.bo, 0, 0, nullptr, 0, nullptr, ch.bytes, ch.bo.mapped, RetireKind::WcShared});
    ch = Chunk();
}

void Device::retireScrubs(DeferredFrees *deferred)
{
    for (auto it = scrubbing_.begin(); it != scrubbing_.end();) {
        const ScrubFate fate = scrubFate(
            [&] {
                return done(it->seqno);
            },
            [&] {
                return channelCurrent(it->generation);
            },
            [&] {
                return killed(it->seqno);
            });
        if (fate == ScrubFate::Waiting) {
            ++it;
            continue;
        }
        if (fate == ScrubFate::Zeroed) {
            scrubbedFree(*it, deferred);
        } else {
            // its job was on a channel since lost, or a kill covered it: it may never have run
            scrubWanted_.push_back(*it);
            scrubDue_.store(true, std::memory_order_relaxed);
        }
        it = scrubbing_.erase(it);
    }
}

void Device::scrubInto(CommandBuffer &cb)
{
    scrubsReturned(cb);
    if (!scrubDue_.load(std::memory_order_relaxed))
        return;
    {
        DeferredFrees deferred(*this);
        std::lock_guard<gm20b::Mutex> l(lock_);
        retireScrubs(&deferred);
        cb.scrubs_.swap(scrubWanted_);
        scrubDue_.store(false, std::memory_order_relaxed);
    }
    if (cb.scrubs_.empty())
        return;
    // VOL=1 memory: a job that ends Volatile (WFI + FB_FLUSH) or External leaves the zeros in DRAM for the CPU's
    // write-combined view
    cb.volatileWrites_ = true;
    for (const Scrub &s : cb.scrubs_)
        cb.useAddress(s.va, s.bytes, true);
    BlitEncoder *b = cb.blit();
    for (const Scrub &s : cb.scrubs_) {
        b->fillWords(s.va, s.bytes, 0);
        SWITCH_METAL_DIAGNOSTIC(stats.scrubbedBytes += s.bytes);
    }
    b->end();
    SWITCH_METAL_DIAGNOSTIC(stats.scrubbedRuns++);
}

void Device::scrubsSubmitted(CommandBuffer &cb, uint32_t seqno)
{
    if (cb.scrubs_.empty())
        return;
    const uint64_t generation = channelGeneration();
    std::lock_guard<gm20b::Mutex> l(lock_);
    for (Scrub &s : cb.scrubs_) {
        s.seqno = seqno;
        s.generation = generation;
        scrubbing_.push_back(s);
    }
    cb.scrubs_.clear();
}

void Device::scrubsReturned(CommandBuffer &cb)
{
    if (cb.scrubs_.empty())
        return;
    std::lock_guard<gm20b::Mutex> l(lock_);
    scrubWanted_.insert(scrubWanted_.end(), cb.scrubs_.begin(), cb.scrubs_.end());
    scrubDue_.store(true, std::memory_order_relaxed);
    cb.scrubs_.clear();
}

bool Device::scrubFreed(uint32_t &seqno, uint64_t &generation)
{
    seqno = 0;
    generation = 0;
    if (!scrubDue_.load(std::memory_order_relaxed))
        return true;
    CommandBuffer *rc = begin();
    if (!rc)
        return false;
    if (rc->scrubs_.empty()) {
        rc->abandon();
        return true;
    }
    rc->internalCompletion();
    uint32_t seq = 0;
    if (!rc->commit(seq)) {
        rc->abandon();
        return false;
    }
    seqno = seq;
    generation = channelGeneration();
    return true;
}

void Device::scrubWait(uint32_t seqno, uint64_t generation)
{
    if (!seqno)
        return;
    (void)wait(seqno, generation);
    DeferredFrees deferred(*this);
    std::lock_guard<gm20b::Mutex> l(lock_);
    retireScrubs(&deferred);
}

}  // namespace gm20b
