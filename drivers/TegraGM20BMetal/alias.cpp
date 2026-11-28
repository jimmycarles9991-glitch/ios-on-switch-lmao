// SPDX-License-Identifier: MIT
// CPU access to the driver's memory, and leases: a heap resource made aliasable (makeArenaAliasable) gives its placement
// back and keeps its arena by a lease, dropped by destroyTexture/destroyBuffer, so the arena is freed only once no work of it
// can still reach it. What teardown detaches is freed synchronously once lock_ is let go (DeferredFrees).
#include "lock.h"
#include "gpu.h"
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <os/log.h>
#include <algorithm>

namespace gm20b {

// A BoFree waits for the connection's submit gate when another thread holds it, and a surface's last release is an IPC
// to the IOSurface kext: neither may hold up threads waiting on lock_. Each caller detaches into its own stack-local
// DeferredFrees and flushes it synchronously after unlocking lock_.
void Device::DeferredFrees::add(const Retired &r)
{
    switch (r.kind) {
    case RetireKind::None:
        break;
    case RetireKind::Chunk:
        dev_.retiring_.chunkBytes.fetch_add(r.accountBytes, std::memory_order_relaxed);
        dev_.retiring_.chunkCount.fetch_add(1, std::memory_order_relaxed);
        break;
    case RetireKind::WcShared:
        dev_.retiring_.wcBytes.fetch_add(r.accountBytes, std::memory_order_relaxed);
        dev_.retiring_.wcSharedBytes.fetch_add(r.accountBytes, std::memory_order_relaxed);
        dev_.retiring_.wcMapped.fetch_add(r.accountMapped, std::memory_order_relaxed);
        break;
    case RetireKind::WcDedicated:
        dev_.retiring_.wcBytes.fetch_add(r.accountBytes, std::memory_order_relaxed);
        dev_.retiring_.wcMapped.fetch_add(r.accountMapped, std::memory_order_relaxed);
        break;
    case RetireKind::SharedHeap:
        dev_.retiring_.heapBytes.fetch_add(r.accountBytes, std::memory_order_relaxed);
        break;
    case RetireKind::GpuHeap:
        dev_.retiring_.gpuHeapBytes.fetch_add(r.accountBytes, std::memory_order_relaxed);
        break;
    case RetireKind::KeptImport:
        dev_.retiring_.keptBytes.fetch_add(r.accountBytes, std::memory_order_relaxed);
        break;
    }
    items_.push_back(r);
}

void Device::DeferredFrees::flush()
{
    if (items_.empty() && !compFreed_)
        return;
    std::vector<Retired> items;
    items.swap(items_);
    const bool compFreed = compFreed_;
    compFreed_ = false;
    for (Retired &r : items)
        dev_.freeNow(r);
    if (compFreed) {
        std::lock_guard<gm20b::Mutex> l(dev_.lock_);
        dev_.compGaveBack();
    }
}

void Device::freeNow(Retired &r)
{
    if (r.bo.handle)
        k_.free(r.bo);
    if (r.va)
        k_.releaseVa(r.va, r.vaBytes);
    if (r.host)
        vm_deallocate(mach_task_self(), vm_address_t(r.host), r.hostBytes);
    if (r.owner)
        CFRelease(r.owner);
    switch (r.kind) {
    case RetireKind::None:
        break;
    case RetireKind::Chunk:
        retiring_.chunkBytes.fetch_sub(r.accountBytes, std::memory_order_relaxed);
        retiring_.chunkCount.fetch_sub(1, std::memory_order_relaxed);
        break;
    case RetireKind::WcShared:
        retiring_.wcBytes.fetch_sub(r.accountBytes, std::memory_order_relaxed);
        retiring_.wcSharedBytes.fetch_sub(r.accountBytes, std::memory_order_relaxed);
        retiring_.wcMapped.fetch_sub(r.accountMapped, std::memory_order_release);
        break;
    case RetireKind::WcDedicated:
        retiring_.wcBytes.fetch_sub(r.accountBytes, std::memory_order_relaxed);
        retiring_.wcMapped.fetch_sub(r.accountMapped, std::memory_order_release);
        break;
    case RetireKind::SharedHeap:
        retiring_.heapBytes.fetch_sub(r.accountBytes, std::memory_order_relaxed);
        break;
    case RetireKind::GpuHeap:
        retiring_.gpuHeapBytes.fetch_sub(r.accountBytes, std::memory_order_relaxed);
        break;
    case RetireKind::KeptImport:
        retiring_.keptBytes.fetch_sub(r.accountBytes, std::memory_order_relaxed);
        break;
    }
}

void reportCpuReleaseFailure()
{
    os_log_error(OS_LOG_DEFAULT, "SwitchMetal: CPU backing release failed");
}

Bo Device::bufferBoLocked(const Buffer &buffer) const
{
    if (buffer.import)
        return buffer.import->bo;
    if (!buffer.span.valid())
        return {};
    const uint32_t index = buffer.span.chunk;
    if (buffer.arena)
        return index < arenas_.size() && !arenas_[index].purged ? arenas_[index].bo : Bo{};
    const auto &chunks = buffer.wc ? (buffer.gpuVolatile ? volatileWcChunks_ : wcChunks_) : chunks_;
    return index < chunks.size() ? chunks[index].bo : Bo{};
}

bool Device::arenaCpuRangesLocked(const ArenaStorage &storage, const void *cpu, uint64_t bytes,
                                  std::vector<HostAccess::Range> &ranges) const
{
    if (storage.arena >= arenas_.size() || !arenas_[storage.arena].used || arenas_[storage.arena].purged ||
        !hostRangeContains(storage.cpu, storage.bytes(), cpu, bytes))
        return false;
    const Arena &arena = arenas_[storage.arena];
    const uint64_t first = storage.va() + (uintptr_t(cpu) - uintptr_t(storage.cpu)), last = first + bytes;
    bool valid = true;
    storage.each([&](const StoragePiece &piece) {
        const uint64_t from = std::max(first, piece.va), to = std::min(last, piece.va + piece.bytes);
        if (!valid || from >= to)
            return;
        const uint64_t offset = storage.offset + from - storage.va();
        Bo bo = arena.bo;
        if (!bo.handle)
            for (const auto &[start, part] : arena.parts)
                if (offset >= start && offset - start < part.bytes && to - from <= part.bytes - (offset - start)) {
                    bo = part;
                    break;
                }
        uint64_t inBo = 0;
        if (!bo.handle || !boCpuOffset(bo, arena.cpu + offset, to - from, inBo)) {
            valid = false;
            return;
        }
        if (!ranges.empty() && ranges.back().bo.handle == bo.handle &&
            ranges.back().offset + ranges.back().bytes == inBo)
            ranges.back().bytes += to - from;
        else
            ranges.push_back({bo, inBo, to - from});
    });
    return valid;
}

void Device::releaseHostArena(void *owner, uint32_t index)
{
    auto &device = *static_cast<Device *>(owner);
    DeferredFrees deferred(device);
    std::unique_lock<gm20b::Mutex> lock(device.lock_);
    device.waitArena(lock, index);
    Arena &arena = device.arenas_[index];
    --arena.leases;
    if (arena.dying && !arena.leases && !arena.aliasLeases)
        device.freeArena(arena, deferred);
}

bool Device::hostArenaAccess(const ArenaStorage &storage, const void *cpu, uint64_t bytes, bool write,
                             HostAccess &access)
{
    std::vector<HostAccess::Range> ranges;
    {
        std::unique_lock<gm20b::Mutex> lock(lock_);
        waitArena(lock, storage.arena);
        if (!arenaCpuRangesLocked(storage, cpu, bytes, ranges))
            return false;
        ++arenas_[storage.arena].leases;
    }
    return access.acquire(k_, std::move(ranges), write, this, storage.arena, releaseHostArena);
}

CpuCompletion Device::captureCpuCompletion(uint32_t seqno, uint64_t generation,
                                           const std::vector<const Buffer *> &buffers)
{
    CpuCompletion result;
    if (buffers.empty())
        return result;
    std::unique_lock<gm20b::Mutex> lock(lock_);
    for (;;) {
        bool waited = false;
        for (const Buffer *buffer : buffers) {
            if (!buffer || !buffer->hostVisible || buffer->wc || !buffer->cpu)
                continue;
            const uint32_t index = buffer->span.storage                    ? buffer->span.storage->arena
                                   : buffer->arena && buffer->span.valid() ? buffer->span.chunk
                                                                           : ~0u;
            if (index < arenas_.size() && arenas_[index].busy) {
                waitArena(lock, index);
                waited = true;
                break;
            }
        }
        if (!waited)
            break;
    }
    const volatile uint32_t *fence = fence_.load(std::memory_order_acquire);
    const uint32_t ch = channel_.load(std::memory_order_acquire);
    if (generation != channelGeneration() || !channelReady_.load(std::memory_order_acquire) || fence == &kNoFence) {
        result.valid = false;
        return result;
    }
    const auto *identity = reinterpret_cast<const volatile TegraGM20BAbiFenceIdentity *>(
        reinterpret_cast<const volatile uint8_t *>(fence) + sizeof(TegraGM20BAbiFence) +
        sizeof(TegraGM20BAbiFenceSemantic));
    result.channelUid = __atomic_load_n(&identity->channel_uid, __ATOMIC_ACQUIRE);
    result.generation = generation;
    result.channel = ch;
    result.seqno = seqno;
    for (const Buffer *buffer : buffers) {
        if (!buffer || !buffer->hostVisible || buffer->wc || !buffer->cpu)
            continue;
        if (buffer->span.storage) {
            std::vector<HostAccess::Range> ranges;
            if (!arenaCpuRangesLocked(*buffer->span.storage, buffer->cpu, buffer->length, ranges)) {
                result.valid = false;
                break;
            }
            for (const HostAccess::Range &range : ranges)
                result.ranges.push_back({range.bo.handle, range.offset, range.bytes});
            continue;
        }
        const Bo bo = bufferBoLocked(*buffer);
        uint64_t offset = 0;
        if (!bo.handle || !buffer->length || !boCpuOffset(bo, buffer->cpu, buffer->length, offset)) {
            result.valid = false;
            break;
        }
        result.ranges.push_back({bo.handle, offset, buffer->length});
    }
    result.coalesce();
    return result;
}

bool Device::hostAccess(const Buffer &buffer, bool write, HostAccess &access)
{
    return hostAccess(buffer, buffer.cpu, buffer.length, write, access);
}

bool Device::hostAccess(const Buffer &buffer, const void *cpu, uint64_t bytes, bool write, HostAccess &access)
{
    if (!hostRangeContains(buffer.cpu, buffer.length, cpu, bytes))
        return false;
    // A write-combined chunk's buffer: its memory holds no CPU line, and the GPU's side is the job's (a host-written use
    // drops the L2's lines first, a write ends External). A kernel CPU lease would sleep on the chunk's GPU work, take it
    // out of the resource set and bump host_epoch. A heap's arena buffer keeps its lease (hostArenaAccess).
    if (buffer.wc && (buffer.gpuVolatile || !buffer.span.storage))
        return true;
    if (buffer.span.storage)
        return hostArenaAccess(*buffer.span.storage, cpu, bytes, write, access);
    Bo bo;
    {
        std::unique_lock<gm20b::Mutex> lock(lock_);
        if (buffer.arena && buffer.span.valid())
            waitArena(lock, buffer.span.chunk);
        bo = bufferBoLocked(buffer);
    }
    return access.acquire(k_, bo, cpu, bytes, write);
}

bool Device::hostAccess(const Texture &texture, bool write, HostAccess &access)
{
    return hostAccess(texture, texture.cpu, 0, write, access);
}

bool Device::hostAccess(const Texture &texture, const void *cpu, uint64_t requestedBytes, bool write,
                        HostAccess &access)
{
    const auto storage = texture.storageRange();
    if (storage.arena) {
        if (!hostRangeContains(storage.arena->cpu, storage.arena->bytes(), cpu, 1))
            return false;
        const uint64_t bytes =
            requestedBytes ? requestedBytes : storage.arena->bytes() - (uintptr_t(cpu) - uintptr_t(storage.arena->cpu));
        return hostArenaAccess(*storage.arena, cpu, bytes, write, access);
    }
    Bo bo;
    uint64_t bytes = 0;
    {
        std::unique_lock<gm20b::Mutex> lock(lock_);
        if (texture.span.valid()) {
            const uint32_t index = texture.span.chunk;
            if (texture.arena) {
                waitArena(lock, index);
                if (index >= arenas_.size() || arenas_[index].purged)
                    return false;
                bo = arenas_[index].bo;
            } else {
                const Memory from = memoryOf(texture);
                const auto &chunks = from == Memory::Bl ? blChunks_ : from == Memory::Wc ? wcChunks_ : chunks_;
                if (index >= chunks.size())
                    return false;
                bo = chunks[index].bo;
            }
            bytes = texture.span.bytes;
        } else if (texture.import) {
            bo = texture.import->bo;
            uint64_t offset = 0;
            if (!boCpuOffset(bo, texture.cpu, 1, offset))
                return false;
            bytes = bo.bytes - offset;
        }
    }
    if (requestedBytes && !hostRangeContains(texture.cpu, bytes, cpu, requestedBytes))
        return false;
    return access.acquire(k_, bo, cpu, requestedBytes ? requestedBytes : bytes, write);
}

struct Lease {
    Device *dev;
    uint32_t refs;
    Memory from;
    Span span;
    Import *import;
    bool placed = true;  // an arena lease's span is its own placement (makeArenaAliasable's is not)
};

Lease *Device::leaseSpan(Lease *&lease, const Span &span, Memory from)
{
    if (!lease) {
        lease = new Lease{this, 1, from, span, nullptr};
        if (from == Memory::Arena)
            arenas_[span.chunk].aliasLeases++;
    }
    lease->refs++;
    return lease;
}

void Device::makeArenaAliasable(Span &span, Lease *&lease)
{
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitArena(l, span.chunk);
    Lease *hold = leaseSpan(lease, span, Memory::Arena);
    arenas_[span.chunk].place.release(span.offset, span.bytes);
    // Aliasing returns placement, while recorded uses still own the mappings and their storage cuts.
    hold->placed = false;
    dropLocked(l, hold, deferred);
}

// An extended-range texture's stand-in was placed in the arena beside it (arenaTexture), counted in its footprint
void Device::makeArenaAliasable(Texture &t)
{
    for (Texture *placed : {&t, t.standIn})
        if (placed && placed->arena)
            makeArenaAliasable(placed->span, placed->lease);
}

void Device::dropLocked(std::unique_lock<gm20b::Mutex> &l, Lease *lease, DeferredFrees &deferred)
{
    if (--lease->refs)
        return;
    switch (lease->from) {
    case Memory::Pool:
        poolFree(lease->span, deferred);
        break;
    case Memory::Wc:
        wcFree(lease->span, deferred);
        break;
    case Memory::VolatileWc:
        volatileWcFree(lease->span, deferred);
        break;
    case Memory::Import:
        release(lease->import, deferred);
        break;
    case Memory::Bl:
        blFree(lease->span, deferred);
        break;
    case Memory::Arena:
        waitArena(l, lease->span.chunk);
        arenas_[lease->span.chunk].aliasLeases--;
        if (lease->placed)
            arenaFree(l, lease->span, deferred);
        else {
            delete lease->span.storage;
            if (arenas_[lease->span.chunk].dying && !arenas_[lease->span.chunk].leases &&
                !arenas_[lease->span.chunk].aliasLeases)
                freeArena(arenas_[lease->span.chunk], deferred);
        }
        break;
    }
    delete lease;
}

void Device::freeSpan(std::unique_lock<gm20b::Mutex> &l, Span &span, Memory from, Lease *&lease,
                      DeferredFrees &deferred)
{
    if (lease) {
        dropLocked(l, lease, deferred);
    } else if (span.valid()) {
        switch (from) {
        case Memory::Pool:
            poolFree(span, deferred);
            break;
        case Memory::Wc:
            wcFree(span, deferred);
            break;
        case Memory::VolatileWc:
            volatileWcFree(span, deferred);
            break;
        case Memory::Import:
            break;
        case Memory::Bl:
            blFree(span, deferred);
            break;
        case Memory::Arena:
            arenaFree(l, span, deferred);
            break;
        }
    }
    span = {};
    lease = nullptr;
}

}  // namespace gm20b
