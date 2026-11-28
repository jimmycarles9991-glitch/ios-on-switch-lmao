// SPDX-License-Identifier: MIT
#include "lock.h"
#include "gpu.h"

#include <algorithm>

#include "gm20b_gpu_internal.h"

namespace gm20b {

std::vector<Device::Chunk> &Device::chunkVec(ChunkVec vec, uint32_t zetaIndex)
{
    switch (vec) {
    case ChunkVec::Pool:
        return chunks_;
    case ChunkVec::Bl:
        return blChunks_;
    case ChunkVec::Zeta:
        return zChunks_[zetaIndex];
    case ChunkVec::Wc:
        return wcChunks_;
    case ChunkVec::VolatileWc:
        return volatileWcChunks_;
    }
    return chunks_;
}

bool Device::hasTransientChunksLocked() const
{
    const auto busy = [](const std::vector<Chunk> &chunks) {
        return std::any_of(chunks.begin(), chunks.end(), [](const Chunk &ch) {
            return ch.park == ChunkPark::Parking || ch.park == ChunkPark::Unparking;
        });
    };
    if (busy(chunks_) || busy(blChunks_) || busy(wcChunks_) || busy(volatileWcChunks_))
        return true;
    for (const std::vector<Chunk> &z : zChunks_)
        if (busy(z))
            return true;
    return false;
}

void Device::collectParkableChunksLocked(std::vector<ParkCandidate> &out)
{
    auto stage = [&](ChunkVec vec, uint32_t zetaIndex, uint32_t index, Chunk &ch) {
        if (ch.park != ChunkPark::Live || ch.used || !ch.bo.handle || ch.bo.control)
            return;
        const bool wc = vec == ChunkVec::Wc || vec == ChunkVec::VolatileWc;
        if (!wc && !ch.memory)
            return;
        const uint64_t mapped = ch.bo.mapped;
        ch.park = ChunkPark::Parking;
        ch.parkedVa = ch.memory ? uint64_t(uintptr_t(ch.memory)) : uint64_t(uintptr_t(ch.bo.cpu));
        ch.memory = nullptr;
        ch.free.clear();
        if (vec == ChunkVec::Pool && !ch.dedicated)
            poolFree_ -= ch.bytes;
        if (wc) {
            retiring_.wcBytes.fetch_add(ch.bytes, std::memory_order_relaxed);
            if (!ch.dedicated)
                retiring_.wcSharedBytes.fetch_add(ch.bytes, std::memory_order_relaxed);
            retiring_.wcMapped.fetch_add(mapped, std::memory_order_relaxed);
        } else {
            retiring_.chunkBytes.fetch_add(ch.bytes, std::memory_order_relaxed);
            retiring_.chunkCount.fetch_add(1, std::memory_order_relaxed);
            SWITCH_METAL_DIAGNOSTIC(stats.poolBytes -= ch.bytes);
            SWITCH_METAL_DIAGNOSTIC(stats.poolChunks--);
        }
        out.push_back({vec, zetaIndex, index, ch.bo, ch.bytes, ch.pitchVa, mapped, ch.dedicated, false});
    };

    for (const Spare &sp : spares_)
        if (sp.chunk < chunks_.size())
            stage(ChunkVec::Pool, 0, sp.chunk, chunks_[sp.chunk]);
    spares_.clear();

    for (uint32_t i = 0; i < chunks_.size(); ++i)
        if (!chunks_[i].dedicated)
            stage(ChunkVec::Pool, 0, i, chunks_[i]);
    for (uint32_t i = 0; i < blChunks_.size(); ++i)
        stage(ChunkVec::Bl, 0, i, blChunks_[i]);
    for (uint32_t z = 0; z < kZetaFormats; ++z)
        for (uint32_t i = 0; i < zChunks_[z].size(); ++i)
            stage(ChunkVec::Zeta, z, i, zChunks_[z][i]);
    for (uint32_t i = 0; i < wcChunks_.size(); ++i)
        stage(ChunkVec::Wc, 0, i, wcChunks_[i]);
    for (uint32_t i = 0; i < volatileWcChunks_.size(); ++i)
        stage(ChunkVec::VolatileWc, 0, i, volatileWcChunks_[i]);
    if (!out.empty())
        ++offLockChunkOps_;
}

void Device::finishParkChunks(std::vector<ParkCandidate> &candidates)
{
    if (candidates.empty())
        return;
    std::vector<uint64_t> handles;
    handles.reserve(candidates.size());
    for (const ParkCandidate &cand : candidates)
        handles.push_back(cand.bo.handle);
    // Kernel BoPark refuses BOs still attached to a resource set.
    set_.remove(k_, handles);

    for (ParkCandidate &cand : candidates) {
        if (k_.park(cand.bo.handle, true) == KERN_SUCCESS) {
            cand.parked = true;
            continue;
        }
        k_.free(cand.bo);
        if (cand.pitchVa)
            k_.releaseVa(cand.pitchVa, cand.bytes);
    }

    std::unique_lock<gm20b::Mutex> l(lock_);
    std::vector<size_t> toFreeOnClose;
    for (size_t i = 0; i < candidates.size(); ++i) {
        ParkCandidate &cand = candidates[i];
        if (cand.vec == ChunkVec::Wc || cand.vec == ChunkVec::VolatileWc) {
            retiring_.wcBytes.fetch_sub(cand.bytes, std::memory_order_relaxed);
            if (!cand.dedicated)
                retiring_.wcSharedBytes.fetch_sub(cand.bytes, std::memory_order_relaxed);
            retiring_.wcMapped.fetch_sub(cand.mapped, std::memory_order_release);
        } else {
            retiring_.chunkBytes.fetch_sub(cand.bytes, std::memory_order_relaxed);
            retiring_.chunkCount.fetch_sub(1, std::memory_order_relaxed);
        }
        Chunk &ch = chunkVec(cand.vec, cand.zetaIndex)[cand.index];
        if (cand.parked && !closing_) {
            ch.park = ChunkPark::Parked;
            ch.bo.parked = true;
            ch.bo.cpu = nullptr;
            ch.bo.mapped = 0;
            SWITCH_METAL_DIAGNOSTIC(stats.parkedChunks++);
            SWITCH_METAL_DIAGNOSTIC(stats.parkedChunkBytes += cand.bytes);
            SWITCH_METAL_DIAGNOSTIC(stats.chunkParks++);
        } else if (cand.parked && closing_) {
            toFreeOnClose.push_back(i);
        } else {
            ch = Chunk();
        }
    }
    // Keep ChunkPark::Parking and offLockChunkOps_ held until all closing_ frees finish so ~Device() cannot race.
    if (!toFreeOnClose.empty()) {
        l.unlock();
        for (size_t idx : toFreeOnClose) {
            ParkCandidate &cand = candidates[idx];
            cand.bo.parked = true;
            cand.bo.cpu = nullptr;
            cand.bo.mapped = 0;
            k_.free(cand.bo);
            if (cand.pitchVa)
                k_.releaseVa(cand.pitchVa, cand.bytes);
        }
        l.lock();
        for (size_t idx : toFreeOnClose) {
            const ParkCandidate &cand = candidates[idx];
            chunkVec(cand.vec, cand.zetaIndex)[cand.index] = Chunk();
        }
    }
    --offLockChunkOps_;
    chunkReady_.notify_all();
}

bool Device::tryUnparkChunk(std::unique_lock<gm20b::Mutex> &l, ChunkVec vec, uint32_t zetaIndex, uint64_t bytes,
                            bool dedicated, uint64_t align, uint32_t setAccess, uint32_t &index, bool &resident,
                            uint64_t maxBytes)
{
    index = ~0u;
    resident = false;
    if (!k_.supportsBoPark() || closing_)
        return false;
    std::vector<Chunk> &chunks = chunkVec(vec, zetaIndex);
    uint32_t best = ~0u;
    for (uint32_t i = 0; i < chunks.size(); ++i) {
        const Chunk &ch = chunks[i];
        if (ch.park != ChunkPark::Parked || ch.used || ch.dedicated != dedicated)
            continue;
        if (dedicated ? ch.bytes != bytes : (ch.bytes < bytes || (maxBytes && ch.bytes > maxBytes)))
            continue;
        if (align && (ch.bo.va & (align - 1)))
            continue;
        if (best == ~0u || ch.bytes < chunks[best].bytes)
            best = i;
    }
    if (best == ~0u)
        return false;

    Chunk &claimed = chunks[best];
    claimed.park = ChunkPark::Unparking;
    ++offLockChunkOps_;
    Bo bo = claimed.bo;
    const uint64_t chBytes = claimed.bytes;
    const uint64_t pitchVa = claimed.pitchVa;
    const bool hostTouched = claimed.hostTouched;
    SWITCH_METAL_DIAGNOSTIC(stats.parkedChunks--);
    SWITCH_METAL_DIAGNOSTIC(stats.parkedChunkBytes -= chBytes);
    l.unlock();

    uint64_t addr = 0, mappedBytes = 0;
    uint32_t status = 0;
    const int kr = k_.unpark(bo.handle, addr, mappedBytes, status);
    if (kr == KERN_SUCCESS && status == TegraGM20BUnparkResident && addr != 0) {
        bo.parked = false;
        bo.cpu = reinterpret_cast<uint8_t *>(uintptr_t(addr));
        bo.mapped = mappedBytes;
    }
    const bool gotResident =
        kr == KERN_SUCCESS && status == TegraGM20BUnparkResident && addr != 0 && mappedBytes >= chBytes;
    if (gotResident) {
        if (!hostTouched) {
            if (pitchVa) {
                Bo view = bo;
                view.va = view.vaBase = pitchVa;
                set_.addLater({{&bo, setAccess}, {&view, TegraGM20BSurfaceWrite}});
            } else {
                set_.addLater({{&bo, setAccess}});
            }
        }
    } else {
        k_.free(bo);
        if (pitchVa)
            k_.releaseVa(pitchVa, chBytes);
    }

    l.lock();
    if (gotResident && !closing_) {
        Chunk &slot = chunkVec(vec, zetaIndex)[best];
        slot.bo = bo;
        slot.memory = bo.cpu;
        slot.parkedVa = 0;
        slot.park = ChunkPark::Live;
        slot.highWater = chBytes;
        if (!dedicated) {
            slot.free.clear();
            slot.free[0] = chBytes;
        }
        if (vec == ChunkVec::Pool && !dedicated)
            poolFree_ += chBytes;
        if (vec != ChunkVec::Wc && vec != ChunkVec::VolatileWc) {
            SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += chBytes);
            SWITCH_METAL_DIAGNOSTIC(stats.poolChunks++);
        }
        SWITCH_METAL_DIAGNOSTIC(stats.chunkUnparksResident++);
        --offLockChunkOps_;
        chunkReady_.notify_all();
        index = best;
        resident = true;
        return true;
    }
    if (gotResident && closing_) {
        // Keep ChunkPark::Unparking and offLockChunkOps_ held across off-lock teardown free.
        l.unlock();
        k_.free(bo);
        if (pitchVa)
            k_.releaseVa(pitchVa, chBytes);
        l.lock();
        chunkVec(vec, zetaIndex)[best] = Chunk();
        --offLockChunkOps_;
        chunkReady_.notify_all();
        return false;
    }
    chunkVec(vec, zetaIndex)[best] = Chunk();
    if (kr == KERN_SUCCESS && status == TegraGM20BUnparkReclaimed)
        SWITCH_METAL_DIAGNOSTIC(stats.chunkUnparksReclaimed++);
    --offLockChunkOps_;
    chunkReady_.notify_all();
    return !closing_;
}

void Device::releaseParkedChunksOnClose()
{
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    chunkReady_.wait(l, [&] {
        return !hasTransientChunksLocked() && offLockChunkOps_ == 0;
    });
    auto detachParked = [&](std::vector<Chunk> &chunks) {
        for (Chunk &ch : chunks) {
            if (ch.park != ChunkPark::Parked)
                continue;
            SWITCH_METAL_DIAGNOSTIC(stats.parkedChunks--);
            SWITCH_METAL_DIAGNOSTIC(stats.parkedChunkBytes -= ch.bytes);
            deferred.add({ch.bo, ch.pitchVa, ch.pitchVa ? ch.bytes : 0});
            ch = Chunk();
        }
    };
    detachParked(chunks_);
    detachParked(blChunks_);
    for (std::vector<Chunk> &z : zChunks_)
        detachParked(z);
    detachParked(wcChunks_);
    detachParked(volatileWcChunks_);
}

}  // namespace gm20b
