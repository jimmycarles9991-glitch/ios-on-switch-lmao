// SPDX-License-Identifier: MIT
// CPU access to the driver's memory, and leases: a heap resource made aliasable (makeArenaAliasable) gives its placement
// back and keeps its arena by a lease, dropped by destroyTexture/destroyBuffer, so the arena is freed only once no work of it
// can still reach it.
#include "lock.h"
#include "gpu.h"
#include <os/log.h>
#include <algorithm>

namespace gm20b {

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
    const auto &chunks = buffer.wc ? wcChunks_ : chunks_;
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
    std::lock_guard<gm20b::Mutex> lock(device.lock_);
    Arena &arena = device.arenas_[index];
    --arena.leases;
    if (arena.dying && !arena.leases)
        device.freeArena(arena);
}

bool Device::hostArenaAccess(const ArenaStorage &storage, const void *cpu, uint64_t bytes, bool write,
                             HostAccess &access)
{
    std::vector<HostAccess::Range> ranges;
    {
        std::lock_guard<gm20b::Mutex> lock(lock_);
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
    std::lock_guard<gm20b::Mutex> lock(lock_);
    if (generation != channelGeneration() || !channelReady_.load(std::memory_order_acquire) || fence_ == &kNoFence) {
        result.valid = false;
        return result;
    }
    const auto *identity = reinterpret_cast<const volatile TegraGM20BAbiFenceIdentity *>(
        reinterpret_cast<const volatile uint8_t *>(fence_) + sizeof(TegraGM20BAbiFence) +
        sizeof(TegraGM20BAbiFenceSemantic));
    result.channelUid = __atomic_load_n(&identity->channel_uid, __ATOMIC_ACQUIRE);
    result.generation = generation;
    result.channel = channel_;
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
    if (buffer.span.storage)
        return hostArenaAccess(*buffer.span.storage, cpu, bytes, write, access);
    Bo bo;
    {
        std::lock_guard<gm20b::Mutex> lock(lock_);
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
        std::lock_guard<gm20b::Mutex> lock(lock_);
        if (texture.span.valid()) {
            const uint32_t index = texture.span.chunk;
            if (texture.arena) {
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
            arenas_[span.chunk].leases++;
    }
    lease->refs++;
    return lease;
}

void Device::makeArenaAliasable(Span &span, Lease *&lease)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    Lease *hold = leaseSpan(lease, span, Memory::Arena);
    arenas_[span.chunk].place.release(span.offset, span.bytes);
    // Aliasing returns placement, while recorded uses still own the mappings and their storage cuts.
    hold->placed = false;
    dropLocked(hold);
}

// An extended-range texture's stand-in was placed in the arena beside it (arenaTexture), counted in its footprint
void Device::makeArenaAliasable(Texture &t)
{
    for (Texture *placed : {&t, t.standIn})
        if (placed && placed->arena)
            makeArenaAliasable(placed->span, placed->lease);
}

void Device::dropLocked(Lease *lease)
{
    if (--lease->refs)
        return;
    switch (lease->from) {
    case Memory::Pool:
        poolFree(lease->span);
        break;
    case Memory::Wc:
        wcFree(lease->span);
        break;
    case Memory::Import:
        release(lease->import);
        break;
    case Memory::Bl:
        blFree(lease->span);
        break;
    case Memory::Arena:
        arenas_[lease->span.chunk].leases--;
        if (lease->placed)
            arenaFree(lease->span);
        else {
            delete lease->span.storage;
            if (arenas_[lease->span.chunk].dying && !arenas_[lease->span.chunk].leases)
                freeArena(arenas_[lease->span.chunk]);
        }
        break;
    }
    delete lease;
}

void Device::freeSpan(Span &span, Memory from, Lease *&lease)
{
    if (lease) {
        dropLocked(lease);
    } else if (span.valid()) {
        switch (from) {
        case Memory::Pool:
            poolFree(span);
            break;
        case Memory::Wc:
            wcFree(span);
            break;
        case Memory::Import:
            break;
        case Memory::Bl:
            blFree(span);
            break;
        case Memory::Arena:
            arenaFree(span);
            break;
        }
    }
    span = {};
    lease = nullptr;
}

}  // namespace gm20b
