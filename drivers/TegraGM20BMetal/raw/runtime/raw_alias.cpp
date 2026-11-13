// SPDX-License-Identifier: MIT
// See raw_gpu.h (Alias). A lease keeps memory an m2v view aliases: a span's lease is made at its first alias and holds one for
// the raw object (dropped by destroyTexture/destroyBuffer), an import's is made per alias; NVK's foreign memory holds one from
// its import to its free. The last drop gives the span back or releases the import, under the device's lock, on whichever
// thread drops it: the raw object's destroy (its command buffers completed) or NVK's free (m2v's use of the view ended), so the
// VA is never reused while either driver's work can still reach it.
#include "raw_lock.h"
#include "raw_gpu.h"
#include <os/log.h>

namespace raw {

void reportCpuReleaseFailure()
{
    os_log_error(OS_LOG_DEFAULT, "raw: CPU backing release failed");
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

CpuCompletion Device::captureCpuCompletion(uint32_t seqno, uint64_t generation,
                                           const std::vector<const Buffer *> &buffers)
{
    CpuCompletion result;
    if (buffers.empty())
        return result;
    std::lock_guard<raw::Mutex> lock(lock_);
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
    Bo bo;
    {
        std::lock_guard<raw::Mutex> lock(lock_);
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
    Bo bo;
    uint64_t bytes = 0;
    {
        std::lock_guard<raw::Mutex> lock(lock_);
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
    bool placed = true;  // an arena lease's span is its own placement (aliasArenaRange's is not)
};

namespace {
void describe(const Bo &bo, uint32_t kind, Lease *lease, Alias &a)
{
    a.cpu = bo.cpu - (bo.va - bo.vaBase);
    a.bytes = bo.bytes;
    a.handle = bo.handle;
    a.va = bo.vaBase;
    a.kind = kind;
    a.lease = lease;
}
}  // namespace

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
        else if (arenas_[lease->span.chunk].dying && !arenas_[lease->span.chunk].leases)
            freeArena(arenas_[lease->span.chunk]);
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

bool Device::alias(Texture &t, Alias &a)
{
    if (!k_.shared() || !t.span.valid())
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    const Memory from = memoryOf(t);
    if (from == Memory::Arena)
        return aliasArena(t.span, t.lease, a);
    const Chunk &ch = (from == Memory::Bl ? blChunks_ : from == Memory::Wc ? wcChunks_ : chunks_)[t.span.chunk];
    describe(ch.bo, from == Memory::Bl ? uint32_t(TGM_LAYOUT_KIND) : kPteKindPitch, leaseSpan(t.lease, t.span, from),
             a);
    return true;
}

bool Device::alias(Buffer &b, Alias &a)
{
    if (!k_.shared())
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    if (b.import) {
        b.import->refs++;
        describe(b.import->bo, b.import->kind, new Lease{this, 1, Memory::Import, {}, b.import}, a);
        return true;
    }
    if (!b.span.valid())
        return false;
    if (b.arena)
        return aliasArena(b.span, b.lease, a);
    const Chunk &ch = (b.wc ? wcChunks_ : chunks_)[b.span.chunk];
    describe(ch.bo, kPteKindPitch, leaseSpan(b.lease, b.span, b.wc ? Memory::Wc : Memory::Pool), a);
    return true;
}

bool Device::aliasArenaRange(uint32_t arena, uint64_t va, uint64_t bytes, Alias &a)
{
    if (!k_.shared())
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].used || arenas_[arena].dying || arenas_[arena].purged)
        return false;
    Arena &ar = arenas_[arena];
    uint64_t base = 0;
    if (!bytes || !arenaKindAt(ar, va, base, a.kind) || bytes > ar.bytes || va - base > ar.bytes - bytes)
        return false;
    uint64_t from = 0, held = ar.bytes, handle = ar.bo.handle;
    for (const auto &[offset, part] : ar.parts)
        if (va - base >= offset && va - base + bytes <= offset + part.bytes) {
            from = offset;
            held = part.bytes;
            handle = part.handle;
        }
    if (!handle)
        return false;  // a trimmed arena's range across two parts: no one BO holds it
    a.cpu = ar.cpu + from;
    a.bytes = held;
    a.handle = handle;
    a.va = base + from;
    a.lease = new Lease{this, 1, Memory::Arena, {arena, va - base, bytes, va, ar.cpu + (va - base)}, nullptr, false};
    ar.leases++;
    return true;
}

bool Device::aliasSurface(uint64_t surfaceId, void *base, uint64_t bytes, const void *surface, Alias &a)
{
    if (!k_.shared() || !surfaceId)
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    Import *i = importSurface(surfaceId, base, bytes, surface);
    if (!i)
        return false;
    describe(i->bo, i->kind, new Lease{this, 1, Memory::Import, {}, i}, a);
    return true;
}

void Device::holdLease(void *lease)
{
    Lease *l = static_cast<Lease *>(lease);
    std::lock_guard<raw::Mutex> g(l->dev->lock_);
    l->refs++;
}

void Device::dropLease(void *lease)
{
    Lease *l = static_cast<Lease *>(lease);
    Device *dev = l->dev;
    std::lock_guard<raw::Mutex> g(dev->lock_);
    dev->dropLocked(l);
}

}  // namespace raw
