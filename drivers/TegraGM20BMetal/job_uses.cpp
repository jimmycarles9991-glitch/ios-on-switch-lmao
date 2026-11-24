// SPDX-License-Identifier: MIT
#include "gpu.h"
#include <algorithm>

namespace gm20b {

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

void CommandBuffer::useStorage(uint64_t va, uint64_t bytes, bool write, bool hostWritten, const ArenaStorage *arena)
{
    if (!va || !bytes || !dev_->k_.exactJobs())
        return;
    const auto name = [&](uint64_t first, uint64_t length) {
        if (useMember(first, length, write, hostWritten))
            return;
        SWITCH_METAL_ENCODE_COUNT(UseAdds);
        surfaces_.addAddress(first, length, write,
                             (hostWritten ? TegraGM20BSurfaceHostWritten : 0u) |
                                 (length <= UINT32_MAX ? uint32_t(TegraGM20BSurfaceRange) : 0u));
    };
    if (!arena || arena->rest.empty()) {
        name(va, bytes);
        return;
    }
    arena->each([&](const StoragePiece &piece) {
        const uint64_t first = std::max(va, piece.va), last = std::min(va + bytes, piece.va + piece.bytes);
        if (first < last)
            name(first, last - first);
    });
}

// lock_ held. A heap's arena is one BO (its parts, once trimmed), named whole for reading (useHeap's resources are
// read-only), host-written for a heap the CPU writes: its write-combined pages leave no CPU line behind, the GPU's L2 may
// hold stale ones. A private heap's arena goes with the set instead. Each resource the heap made outside its arena is a use
// of its own.
void CommandBuffer::nameHeaps(std::unique_lock<gm20b::Mutex> &l)
{
    Device &d = *dev_;
    for (const HeapMembers *heap : heaps_) {
        const uint32_t written = heap->hostVisible() ? uint32_t(TegraGM20BSurfaceHostWritten) : 0u;
        const auto name = [&](const Bo &bo) {
            if (bo.handle && !d.member(bo)) {
                SWITCH_METAL_ENCODE_COUNT(UseAdds);
                surfaces_.addAccess(bo.handle, TegraGM20BSurfaceRead | written);
            }
        };
        if (heap->arena() < d.arenas_.size()) {
            d.waitArena(l, heap->arena());
            const Device::Arena &a = d.arenas_[heap->arena()];
            if (a.used && !a.purged) {
                if (a.multiKindGen && a.multiKindGen->load(std::memory_order_acquire) != 0)
                    arenaKindInvalidate_ = true;
                name(a.bo);
                for (const auto &[offset, part] : a.parts)
                    name(part);
            }
        }
        heap->each([&](const HeapMember &m) {
            SWITCH_METAL_ENCODE_COUNT(UseAdds);
            if (!d.member(m.va, m.bytes))
                surfaces_.addAddress(m.va, m.bytes, false,
                                     (m.hostVisible && !m.gpuVolatile ? uint32_t(TegraGM20BSurfaceHostWritten) : 0u) |
                                         (m.bytes <= UINT32_MAX ? uint32_t(TegraGM20BSurfaceRange) : 0u));
        });
    }
}

void CommandBuffer::useAddress(uint64_t va, uint64_t bytes, bool write)
{
    SWITCH_METAL_ENCODE_SPAN(Use);
    SWITCH_METAL_ENCODE_COUNT(Uses);
    if (va && write && inRender_)
        passBufferWrites_ = true;
    if (va && dev_->k_.exactJobs() && !useMember(va, bytes ? bytes : 1, write, false)) {
        SWITCH_METAL_ENCODE_COUNT(UseAdds);
        surfaces_.addAddress(va, bytes ? bytes : 1, write, bytes && bytes <= UINT32_MAX ? TegraGM20BSurfaceRange : 0u);
    }
}

bool CommandBuffer::hasEncodedWork() const
{
    if (!segments_.empty() || encodersOpened_ > 1)
        return true;
    return activeBlit_ && activeBlit_->emitted();
}

void CommandBuffer::requestKindSwitch()
{
    kindSwitchGen_++;
    if (activeBlit_) {
        activeBlit_->crossKinds();
        pendingKindSwitch_ = false;
    } else {
        pendingKindSwitch_ = true;
    }
}

void CommandBuffer::noteArenaKind(const ArenaStorage *arena)
{
    if (!arena || !arena->multiKindGen)
        return;
    const uint64_t gen = arena->multiKindGen->load(std::memory_order_acquire);
    if (!gen)
        return;
    const uint64_t bytes = arena->bytes();
    KindRanges &cbRanges = cbArenaKinds_[arena->arenaId];
    const bool sameCbCross = cbRanges.overlapsOther(arena->offset, bytes, arena->kind, kindSwitchGen_);
    bool priorCbCross = false;
    if (!sameCbCross) {
        if (auto it = dev_->arenaKindUses_.find(arena->arenaId); it != dev_->arenaKindUses_.end())
            priorCbCross = it->second.overlapsOther(arena->offset, bytes, arena->kind, dev_->l2InvalidatedBeforeCb_);
    }
    const uint64_t seenDev = dev_->arenaCrossSeen(arena->arenaId);
    const uint64_t seenCb = cbCrossSeen_[arena->arenaId];
    const bool unseenCross = gen > std::max(seenDev, seenCb);
    if (sameCbCross) {
        requestKindSwitch();
    } else if (priorCbCross) {
        if (!arenaKindInvalidate_ && !kindSwitchGen_)
            arenaKindInvalidate_ = true;
    } else if (unseenCross) {
        if (hasEncodedWork())
            requestKindSwitch();
        else
            arenaKindInvalidate_ = true;
    }
    if (unseenCross)
        cbCrossSeen_[arena->arenaId] = gen;
    cbRanges.record(arena->offset, bytes, arena->kind, kindSwitchGen_);
}

void CommandBuffer::use(const Buffer &buffer, bool write, bool noted)
{
    SWITCH_METAL_ENCODE_COUNT(Uses);
    // Ahead of the memo: a buffer named once is written again after a release, which then has host-visible data to flush
    publication_.wrote(write && buffer.hostVisible);
    // ahead of the memo too: a buffer named written by a draw that writes nothing is written by a later one
    if (write && noted) {
        bufferWrites_.note(buffer.va, buffer.length);
        if (inRender_)
            passBufferWrites_ = true;
    }
    noteArenaKind(buffer.span.storage);
    if (named_.named(&buffer, buffer.va, buffer.length, buffer.import, write))
        return;
    SWITCH_METAL_ENCODE_SPAN(Use);
    const bool cachedHost = buffer.hostVisible && !buffer.gpuVolatile;
    externalWrites_ |= write && cachedHost;
    volatileWrites_ |= write && buffer.hostVisible && buffer.gpuVolatile;
    const bool ranged = dev_->k_.exactJobs() && buffer.length && buffer.length <= UINT32_MAX;
    if (!ranged || !importContains(buffer.import, buffer.va, buffer.length))
        use(buffer.import, write);
    useStorage(buffer.va, buffer.length, write, cachedHost, buffer.span.storage);
}

void CommandBuffer::use(const Texture &texture, bool write)
{
    SWITCH_METAL_ENCODE_COUNT(Uses);
    const auto range = texture.storageRange();
    if (write) {
        publication_.wrote(range.hostVisible || (texture.import && !texture.import->id) || texture.callerPages);
        zcullPlanes_.wrote(range.va ? range.va : texture.va, range.bytes ? range.bytes : 1);
    }
    noteArenaKind(range.arena);
    if (named_.named(&texture, texture.va, range.bytes, texture.import, write))
        return;
    SWITCH_METAL_ENCODE_SPAN(Use);
    const Texture *view = write ? &texture : readTexture(&texture);
    const auto storage = view->storageRange();
    const bool ranged = dev_->k_.exactJobs() && storage.bytes && storage.bytes <= UINT32_MAX;
    if (!ranged || !importContains(view->import, storage.va, storage.bytes))
        use(view->import, write);
    const bool hostVisible = storage.hostVisible || (view->import && !view->import->id);
    const bool gpuVolatile = storage.gpuVolatile && !(view->import && !view->import->id);
    const bool cachedHost = hostVisible && !gpuVolatile;
    // a write into a copied no-copy texture goes on to its caller's pages (TegraGM20BMetalEncodeBlit.mm copyToCallerPages)
    externalWrites_ |= (cachedHost || texture.callerPages) && write;
    volatileWrites_ |= write && hostVisible && gpuVolatile && !texture.callerPages;
#if SWITCH_METAL_DIAGNOSTICS
    if (write && dev_->glassMemLive_)
        dev_->glassMem->wrote(storage.va ? storage.va : view->va, dev_->commitCount_,
                              dev_->stats.memEvents.load(std::memory_order_relaxed));
#endif
    if (view->va)
        useStorage(storage.bytes ? storage.va : view->va, storage.bytes ? storage.bytes : 1, write, cachedHost,
                   storage.arena);
}

}  // namespace gm20b
