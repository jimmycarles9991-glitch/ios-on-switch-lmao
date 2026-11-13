// SPDX-License-Identifier: MIT
#include "raw_gpu.h"

namespace raw {

static bool importContains(const Import *import, uint64_t va, uint64_t bytes)
{
    if (!import || !bytes)
        return false;
    const uint64_t base = import->bo.vaBase ? import->bo.vaBase : import->bo.va;
    return va >= base && va - base <= import->bo.bytes && bytes <= import->bo.bytes - (va - base);
}

// A use of a member goes with the set and is only recorded, should the member leave the set before the job is submitted
bool CommandBuffer::useMember(uint64_t va, uint64_t bytes, bool write, bool hostWritten)
{
    const SetMember *m = dev_->member(va, bytes);
    if (!m)
        return false;
    if (write && !(m->access & TegraGM20BSurfaceWrite))
        dev_->set_.promote(dev_->k_, m->handle);
    memberUses_.push_back({m->handle, write ? uint32_t(TegraGM20BSurfaceWrite) : uint32_t(TegraGM20BSurfaceRead)});
    memberHostWritten_ |= hostWritten;
    return true;
}

// lock_ held. A heap's arena is one BO (its parts, once trimmed), named whole for reading (useHeap's resources are
// read-only), host-written for a heap the CPU writes: its write-combined pages leave no CPU line behind, the GPU's L2 may
// hold stale ones. A private heap's arena goes with the set instead. Each resource the heap made outside its arena is a use
// of its own.
void CommandBuffer::nameHeaps()
{
    Device &d = *dev_;
    for (const HeapMembers *heap : heaps_) {
        const uint32_t written = heap->hostVisible() ? uint32_t(TegraGM20BSurfaceHostWritten) : 0u;
        const auto name = [&](const Bo &bo) {
            if (bo.handle && !d.member(bo)) {
                RAW_ENCODE_COUNT(UseAdds);
                surfaces_.addAccess(bo.handle, TegraGM20BSurfaceRead | written);
            }
        };
        if (heap->arena() < d.arenas_.size()) {
            const Device::Arena &a = d.arenas_[heap->arena()];
            if (a.used && !a.purged) {
                name(a.bo);
                for (const auto &[offset, part] : a.parts)
                    name(part);
            }
        }
        heap->each([&](const HeapMember &m) {
            RAW_ENCODE_COUNT(UseAdds);
            if (!d.member(m.va, m.bytes))
                surfaces_.addAddress(m.va, m.bytes, false,
                                     (m.hostVisible ? uint32_t(TegraGM20BSurfaceHostWritten) : 0u) |
                                         (m.bytes <= UINT32_MAX ? uint32_t(TegraGM20BSurfaceRange) : 0u));
        });
    }
}

void CommandBuffer::useAddress(uint64_t va, uint64_t bytes, bool write)
{
    RAW_ENCODE_SPAN(Use);
    RAW_ENCODE_COUNT(Uses);
    if (va && dev_->k_.exactJobs() && !useMember(va, bytes ? bytes : 1, write, false)) {
        RAW_ENCODE_COUNT(UseAdds);
        surfaces_.addAddress(va, bytes ? bytes : 1, write, bytes && bytes <= UINT32_MAX ? TegraGM20BSurfaceRange : 0u);
    }
}

void CommandBuffer::use(const Buffer &buffer, bool write)
{
    RAW_ENCODE_COUNT(Uses);
    // Ahead of the memo: a buffer named once is written again after a release, which then has host-visible data to flush
    publication_.wrote(write && buffer.hostVisible);
    if (named_.named(&buffer, buffer.va, buffer.length, buffer.import, write))
        return;
    RAW_ENCODE_SPAN(Use);
    buffersWritten_ |= write;
    externalWrites_ |= write && buffer.hostVisible;
    const bool ranged = dev_->k_.exactJobs() && buffer.length && buffer.length <= UINT32_MAX;
    if (!ranged || !importContains(buffer.import, buffer.va, buffer.length))
        use(buffer.import, write);
    if (buffer.va && buffer.length && dev_->k_.exactJobs() &&
        !useMember(buffer.va, buffer.length, write, buffer.hostVisible)) {
        RAW_ENCODE_COUNT(UseAdds);
        surfaces_.addAddress(buffer.va, buffer.length, write,
                             (buffer.hostVisible ? TegraGM20BSurfaceHostWritten : 0u) |
                                 (ranged ? TegraGM20BSurfaceRange : 0u));
    }
}

void CommandBuffer::use(const Texture &texture, bool write)
{
    RAW_ENCODE_COUNT(Uses);
    const auto range = texture.storageRange();
    if (write)
        publication_.wrote(range.hostVisible || (texture.import && !texture.import->id) || texture.callerPages);
    if (named_.named(&texture, texture.va, range.bytes, texture.import, write))
        return;
    RAW_ENCODE_SPAN(Use);
    const Texture *view = write ? &texture : readTexture(&texture);
    const auto storage = view->storageRange();
    const bool ranged = dev_->k_.exactJobs() && storage.bytes && storage.bytes <= UINT32_MAX;
    if (!ranged || !importContains(view->import, storage.va, storage.bytes))
        use(view->import, write);
    const bool hostVisible = storage.hostVisible || (view->import && !view->import->id);
    // a write into a copied no-copy texture goes on to its caller's pages (RawMetalEncodeBlit.mm copyToCallerPages)
    externalWrites_ |= (hostVisible || texture.callerPages) && write;
#if SWITCH_RAW_DIAGNOSTICS
    if (write && dev_->glassMemLive_)
        dev_->glassMem->wrote(storage.va ? storage.va : view->va, dev_->commitCount_,
                              dev_->stats.memEvents.load(std::memory_order_relaxed));
#endif
    if (view->va && dev_->k_.exactJobs() &&
        !useMember(storage.bytes ? storage.va : view->va, storage.bytes ? storage.bytes : 1, write, hostVisible)) {
        RAW_ENCODE_COUNT(UseAdds);
        surfaces_.addAddress(storage.bytes ? storage.va : view->va, storage.bytes ? storage.bytes : 1, write,
                             (hostVisible ? TegraGM20BSurfaceHostWritten : 0u) |
                                 (ranged ? TegraGM20BSurfaceRange : 0u));
    }
}

}  // namespace raw
