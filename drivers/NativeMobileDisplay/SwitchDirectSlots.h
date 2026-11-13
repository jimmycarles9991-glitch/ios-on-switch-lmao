// SPDX-License-Identifier: BSD-2-Clause-Patent
// The slot entry holding memory (mapped now, into the least recently used slot neither scanned nor asked for, if not yet);
// kNoBuffer when every slot is in use or the pages cannot be had.
int directSlotFor(OSObject *surface, IOMemoryDescriptor *memory, uint64_t stride, uint64_t allocation, bool tiled)
{
    SwitchNativeScanout snapshot = {};
    if (switch_native_scanout_pitch(surface, &snapshot))
        return kNoBuffer;
    const uint64_t offset = snapshot.pitchOffset;
    const uint32_t pages = static_cast<uint32_t>((allocation + 4095) / 4096);
    if ((offset & 4095) || offset / 4096 > snapshot.allocation.page_count ||
        pages > snapshot.allocation.page_count - offset / 4096) {
        switch_native_scanout_release(&snapshot);
        return kNoBuffer;
    }
    int victim = -1;
    for (unsigned k = 0; k < kSlots; ++k) {
        auto &slot = slots[k];
        if (slot.memory == memory && slot.stride == stride && slot.pages == pages && slot.sourceOffset == offset &&
            slot.allocationId == snapshot.allocation.allocation_id &&
            slot.allocationGeneration == snapshot.allocation.allocation_generation) {
            if (slot.read && slot.read->lease) {
                if (slot.read->allocation.content_generation != snapshot.allocation.content_generation) {
                    switch_native_scanout_release(&snapshot);
                    return kNoBuffer;
                }
                switch_native_scanout_release(&snapshot);
            } else {
                *slot.read = snapshot;
            }
            slot.used = ++slotUses;
            slot.tiled = holdLayout(slot, tiled);
            return kSlot0 + static_cast<int>(k);
        }
        if (!ring.busy(kSlot0 + static_cast<int>(k)) && (victim < 0 || slots[k].used < slots[victim].used))
            victim = static_cast<int>(k);
    }
    if (victim < 0) {
        switch_native_scanout_release(&snapshot);
        return kNoBuffer;
    }
    auto &slot = slots[victim];
    if (!slot.read)
        slot.read = IONewZero(SwitchNativeScanout, 1);
    if (!slot.read) {
        switch_native_scanout_release(&snapshot);
        return kNoBuffer;
    }
    dropSlot(static_cast<unsigned>(victim));
    const uint64_t iova = kProbeSlot + static_cast<uint64_t>(victim) * kSlotSpan;
    if (memory->prepare(kIODirectionOut) != kIOReturnSuccess) {
        switch_native_scanout_release(&snapshot);
        return kNoBuffer;
    }
    const bool ok = probeDomain.map(iova, snapshot.allocation.pages + offset / 4096, pages, false);
    if (!ok) {
        probeDomain.unmap(iova, pages);
        cleanDomain();
        memory->complete(kIODirectionOut);
        switch_native_scanout_release(&snapshot);
        return kNoBuffer;
    }
    memory->retain();
    surface->retain();
    auto *storage = slot.read;
    slot = {memory,
            surface,
            nullptr,
            stride,
            ++slotUses,
            pages,
            tiled,
            storage,
            snapshot.allocation.allocation_id,
            snapshot.allocation.allocation_generation,
            offset};
    *slot.read = snapshot;
    holdScanoutBacking(static_cast<unsigned>(victim));
    slot.tiled = holdLayout(slot, tiled);
    cleanDomain();
    return kSlot0 + victim;
}

// The slot's layout held for as long as the slot lives, once its surface's GPU client has decided it; the layout the
// kick read until then.
static bool holdLayout(DirectSlot &slot, bool tiled)
{
    if (!slot.layout) {
        IOByteCount length = 0;
        const addr64_t pa = slot.memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
        int held = 0;
        slot.layout = pa ? tegra_gm20b_surface_hold(pa & ~0xfffULL, &held) : nullptr;
        if (slot.layout)
            return held != 0;
    }
    return slot.layout ? slot.tiled : tiled;
}

// The GPU driver's word on the surface's pixels (by its first page): the block height of its block-linear layout, -1
// when they are rows.
static int surfaceBlockLog2(IOMemoryDescriptor *memory)
{
    IOByteCount length = 0;
    const addr64_t pa = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    tgm_layout layout = {};
    return (pa != 0u) && tegra_gm20b_surface_layout(pa & ~0xfffULL, &layout) == 1 ? layout.y_log2 : -1;
}

// A slot's mapping and its surface given back (it is neither scanned nor asked for).
void dropSlot(unsigned k)
{
    releaseScanoutBacking(k);
    DirectSlot &slot = slots[k];
    if (slot.pages)
        probeDomain.unmap(kProbeSlot + static_cast<uint64_t>(k) * kSlotSpan, slot.pages);
    if (slot.layout)
        tegra_gm20b_surface_unhold(slot.layout);
    if (slot.memory) {
        slot.memory->complete(kIODirectionOut);
        slot.memory->release();
    }
    OSSafeReleaseNULL(slot.surface);
    auto *storage = slot.read;
    if (storage)
        switch_native_scanout_release(storage);
    slot = {};
    slot.read = storage;
    cleanDomain();
}

void releaseIdleDirectReads()
{
    for (unsigned k = 0; k < kSlots; ++k) {
        const int entry = kSlot0 + static_cast<int>(k);
        if (slots[k].read && !ring.busy(entry) && gpuDirect != entry && gpuPresentedEntry != entry)
            switch_native_scanout_release(slots[k].read);
    }
}
