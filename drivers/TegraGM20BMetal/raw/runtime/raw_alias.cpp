// SPDX-License-Identifier: MIT
// See raw_gpu.h (Alias). A lease keeps memory an m2v view aliases: a span's lease is made at its first alias and holds one for
// the raw object (dropped by destroyTexture/destroyBuffer), an import's is made per alias; NVK's foreign memory holds one from
// its import to its free. The last drop gives the span back or releases the import, under the device's lock, on whichever
// thread drops it: the raw object's destroy (its command buffers completed) or NVK's free (m2v's use of the view ended), so the
// VA is never reused while either driver's work can still reach it.
#include "raw_gpu.h"

namespace raw {

struct Lease {
    Device *dev;
    uint32_t refs;
    Memory from;
    Span span;
    Import *import;
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
    if (!lease)
        lease = new Lease{this, 1, from, span, nullptr};
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
        }
    }
    span = {};
    lease = nullptr;
}

bool Device::alias(Texture &t, Alias &a)
{
    if (!k_.shared() || !t.span.valid())
        return false;
    std::lock_guard<std::mutex> l(lock_);
    const Memory from = memoryOf(t);
    const Chunk &ch = (from == Memory::Bl ? blChunks_ : from == Memory::Wc ? wcChunks_ : chunks_)[t.span.chunk];
    describe(ch.bo, from == Memory::Bl ? uint32_t(TGM_LAYOUT_KIND) : kPteKindPitch, leaseSpan(t.lease, t.span, from),
             a);
    return true;
}

bool Device::alias(Buffer &b, Alias &a)
{
    if (!k_.shared())
        return false;
    std::lock_guard<std::mutex> l(lock_);
    if (b.import) {
        b.import->refs++;
        describe(b.import->bo, b.import->kind, new Lease{this, 1, Memory::Import, {}, b.import}, a);
        return true;
    }
    if (!b.span.valid())
        return false;
    const Chunk &ch = (b.wc ? wcChunks_ : chunks_)[b.span.chunk];
    describe(ch.bo, kPteKindPitch, leaseSpan(b.lease, b.span, b.wc ? Memory::Wc : Memory::Pool), a);
    return true;
}

bool Device::aliasSurface(uint64_t surfaceId, void *base, uint64_t bytes, const void *surface, Alias &a)
{
    if (!k_.shared() || !surfaceId)
        return false;
    std::lock_guard<std::mutex> l(lock_);
    Import *i = importSurface(surfaceId, base, bytes, surface);
    if (!i)
        return false;
    describe(i->bo, i->kind, new Lease{this, 1, Memory::Import, {}, i}, a);
    return true;
}

void Device::holdLease(void *lease)
{
    Lease *l = static_cast<Lease *>(lease);
    std::lock_guard<std::mutex> g(l->dev->lock_);
    l->refs++;
}

void Device::dropLease(void *lease)
{
    Lease *l = static_cast<Lease *>(lease);
    Device *dev = l->dev;
    std::lock_guard<std::mutex> g(dev->lock_);
    dev->dropLocked(l);
}

}  // namespace raw
