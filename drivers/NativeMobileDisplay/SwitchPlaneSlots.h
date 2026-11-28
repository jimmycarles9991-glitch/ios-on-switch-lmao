// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's window B slots: included in the class body; runs under the family's work-loop gate.
//
// A slot's mapping is placed in window B's IOVA space (planeSpace, from kPlaneSlot0) at the size it reads, so window B
// keeps as many mappings as its slots and that space hold, and a decoder's pool stays mapped whole. A new mapping takes
// the least recently used slot no set uses (an empty one first) and, while the space cannot fit it, idle mappings are
// dropped in least recently used order; what a set uses keeps its pages and its IOVA.
//
// An engine allocation stays mapped after the frame it showed retires. Its lease goes back at that latch, so the decoder
// recycles the buffer into its next picture and a GPU or VIC writer renders into it again; that next frame reuses the
// mapping (planeMapSnapshot). Allocation IDs are never reused and a backing's page list changes only with its
// generation, so a slot's key names exactly the pages it maps. A slot without a lease is in no set: window B never
// reads pages a freed allocation gave back. When the backing registry frees one, the slot mapping it goes at the gate's
// next latch or placement (planeForgets), and its IOVA is free for the next mapping; until then no set uses the slot.

uint32_t planeSlotIova(int k) const
{
    return planeSlots[k].iova;
}

// The least recently used slot no set uses, other than avoid, an empty one first; with mapped, one holding a mapping.
// -1: none.
int planeSlotIdle(int avoid, bool mapped) const
{
    int lru = -1;
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        const PlaneSlot &slot = planeSlots[k];
        if (int(k) == avoid || (mapped && !slot.iova) || (lru >= 0 && slot.used >= planeSlots[lru].used) ||
            planeSlotBusy(int(k)))
            continue;
        lru = int(k);
    }
    return lru;
}

static inline bool planeUnplacedLogged = false;
static inline switch_display::PlaneForgets<kPlaneSlots> planeForgets;

// The backing registry's retirement thread, once for each freed allocation the display noted: no lock, no wait.
static void planeForgotten(uint64_t allocation)
{
    planeForgets.forget(allocation);
}

// Every slot mapping an allocation the registry freed goes; one a set still uses goes at the latch that frees it.
void planeForgetDrain()
{
    planeForgets.drain([this](uint64_t allocation) {
        for (unsigned k = 0; k < kPlaneSlots; ++k) {
            if (planeSlots[k].mappedAllocation != allocation)
                continue;
            if (planeSlotBusy(int(k)))
                planeSlots[k].forgotten = true;
            else
                planeDropSlot(k);
        }
    });
}

// A slot for a new mapping of [low, high), its IOVA placed: -11 while sets hold every slot, or all the room the space
// has, and the frame waits for a latch to give some back.
int planeSlotPlace(uint64_t low, uint64_t high, int avoid)
{
    planeForgetDrain();
    const int k = planeSlotIdle(avoid, false);
    uint64_t iova = 0;
    for (int drop = k; drop >= 0; drop = planeSlotIdle(avoid, true)) {
        planeNoteDropped(planeSlots[drop]);
        planeDropSlot(static_cast<unsigned>(drop));
        iova = planeSpace.alloc(high - low);
        if (iova)
            break;
    }
    if (!iova) {
        if (!planeUnplacedLogged) {
            planeUnplacedLogged = true;
            IOLog(
                "SwitchDisplay: window B cannot place a %llu KiB mapping: sets hold %s; the frame waits for a latch\n",
                static_cast<unsigned long long>((high - low) >> 10),
                k < 0 ? "every slot" : "the rest of its IOVA space");
        }
        return -11;
    }
    PlaneSlot &slot = planeSlots[k];
    slot.iova = static_cast<uint32_t>(iova);
    slot.low = low;
    slot.high = high;
    return k;
}

#if SWITCH_FRAME_TIMELINE
static inline switch_display::PlaneDropped planeDropped;

void planeNoteDropped(const PlaneSlot &slot)
{
    planeDropped.note(slot.mappedAllocation, slot.mappedGeneration);
}

void planeNoteMapped(uint64_t allocation, uint64_t generation)
{
    if (planeDropped.take(allocation, generation))
        ++stPlaneRemaps;
}
#else
void planeNoteDropped(const PlaneSlot &)
{}

void planeNoteMapped(uint64_t, uint64_t)
{}
#endif

void planeReleaseScanoutBacking(unsigned k)
{
    if (!planeSlots[k].scanoutBacking)
        return;
    switch_gpu_backing_scanout(planeSlots[k].scanoutBacking, 0);
    switch_gpu_backing_release(planeSlots[k].scanoutBacking);
    planeSlots[k].scanoutBacking = nullptr;
}

void planeHoldScanoutBacking(unsigned k, switch_gpu_backing *backing)
{
    planeReleaseScanoutBacking(k);
    if (!backing)
        return;
    switch_gpu_backing_retain(backing);
    switch_gpu_backing_scanout(backing, 1);
    planeSlots[k].scanoutBacking = backing;
}

// The slot holding [low, high) of the surface's memory (mapped now, if not yet, where planeSlotPlace puts it): -1 when
// no slot or room can be had, the pages cannot be had, or the layout the GPU holds the surface in is not the one the
// window's registers were made for (`tiled`: block-linear). A surface the CPU may have written (RGB: an image decoded
// in software) is cleaned to DRAM once when it is mapped: DC A does not snoop the CPU's caches. Video comes from VIC (or a
// CPU copy that cleans) already in DRAM [V: r17-video TegraVICPresent]. surface: the IOSurface the memory is, held with
// it (none for the display's own buffers).
int planeSlotFor(IOMemoryDescriptor *memory, uint64_t low, uint64_t high, bool clean, OSObject *surface = nullptr,
                 bool tiled = false)
{
    if (!memory || !memory->metaCast("IOMemoryDescriptor") || high <= low || high - low > kPlaneSlotSpan)
        return -1;
    const uint64_t pages = (memory->getLength() + 4095) & ~4095ULL;
    const uint64_t end = high < pages ? high : pages;
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        if (planeSlots[k].memory == memory && planeSlots[k].low == low && planeSlots[k].high == end &&
            planeSlots[k].tiled == tiled) {
            addr64_t first = planeSlots[k].firstPa;
            if (!first) {
                IOByteCount length = 0;
                first = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
                planeSlots[k].firstPa = first;
            }
            if (first && !planeSlots[k].scanoutBacking) {
                if (auto *backing = switch_gpu_backing_find_pa(first & ~0xfffULL)) {
                    planeHoldScanoutBacking(k, backing);
                    switch_gpu_backing_release(backing);
                }
            }
            if (first && !planeSlots[k].layout) {
                int held = 0;
                planeSlots[k].layout = tegra_gm20b_surface_hold(first & ~0xfffULL, &held);
                if (planeSlots[k].layout && (held != 0) != tiled) {
                    if (!planeSlotBusy(int(k)))
                        planeDropSlot(k);
                    return -1;
                }
            }
            if (first && tegra_gm20b_surface_compressed(first & ~0xfffULL) != 0) {
                if (!planeSlotBusy(int(k)))
                    planeDropSlot(k);
                return -1;
            }
            planeSlots[k].used = ++planeSlotUses;
            return static_cast<int>(k);
        }
    }
    const int k = end > low ? planeSlotPlace(low, end, -1) : -1;
    if (k < 0)
        return -1;
    PlaneSlot &slot = planeSlots[k];
    if (memory->prepare(kIODirectionOut) != kIOReturnSuccess) {
        planeDropSlot(static_cast<unsigned>(k));
        return -1;
    }
    uint64_t batch[64];
    bool ok = true;
    addr64_t first = 0;
    uint64_t segPa = 0;
    uint64_t segRem = 0;
    for (uint64_t at = low; ok && at < end;) {
        uint32_t n = 0;
        const uint64_t from = at;
        while (n < 64 && at < end) {
            if (!segRem) {
                IOByteCount length = 0;
                const uint64_t pa = memory->getPhysicalSegment(at, &length, kIOMemoryMapperNone);
                if (!pa || (pa & 4095)) {
                    ok = false;
                    break;
                }
                if (at == 0)
                    first = pa;
                segPa = pa;
                segRem = length >= 4096 ? (length & ~4095ULL) : 4096ULL;
            }
            batch[n++] = segPa;
            segPa += 4096;
            segRem -= 4096;
            at += 4096;
        }
        ok = ok && probeDomain.map(slot.iova + (from - low), batch, n, false);
        if (ok && clean && n)
            switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, batch, n);
    }
    if (!ok) {
        planeDropSlot(static_cast<unsigned>(k));
        memory->complete(kIODirectionOut);
        return -1;
    }
    if (clean)
        switch_dcache_sync();
    if (!first) {
        IOByteCount length = 0;
        first = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    }
    memory->retain();
    if (surface)
        surface->retain();
    slot.memory = memory;
    slot.surface = surface;
    slot.firstPa = first;
    slot.used = ++planeSlotUses;
    slot.tiled = tiled;
    cleanDomain();
    int held = 0;
    if (first) {
        if (auto *backing = switch_gpu_backing_find_pa(first & ~0xfffULL)) {
            planeHoldScanoutBacking(static_cast<unsigned>(k), backing);
            switch_gpu_backing_release(backing);
        }
    }
    slot.layout = first ? tegra_gm20b_surface_hold(first & ~0xfffULL, &held) : nullptr;
    const bool compressed = first && tegra_gm20b_surface_compressed(first & ~0xfffULL) != 0;
    if ((slot.layout && (held != 0) != tiled) || compressed) {
        planeDropSlot(static_cast<unsigned>(k));
        return -1;
    }
    if (stats)
        ++stPlaneMaps;
    return k;
}

void planeDropSlot(unsigned k)
{
    planeUseRelease(k);
    planeReleaseScanoutBacking(k);
    PlaneSlot &slot = planeSlots[k];
    if (slot.mappedAllocation)
        planeForgets.publish(k, 0);
    if (slot.iova) {
        probeDomain.unmap(slot.iova, static_cast<uint32_t>((slot.high - slot.low) / 4096));
        planeSpace.release(slot.iova, slot.high - slot.low);
        if (slot.layout)
            tegra_gm20b_surface_unhold(slot.layout);
        if (slot.memory) {
            slot.memory->complete(kIODirectionOut);
            slot.memory->release();
        }
        if (slot.native) {
            switch_native_scanout_deliver(slot.native);
            switch_native_scanout_release(slot.native);
        }
        cleanDomain();
    }
    OSSafeReleaseNULL(slot.surface);
    auto *storage = slot.native;
    slot = {};
    slot.native = storage;
}

void planeReleaseLease(unsigned k)
{
    planeUseRelease(k);
    planeReleaseScanoutBacking(k);
    auto *native = planeSlots[k].native;
    if (native && native->lease) {
        switch_native_scanout_deliver(native);
        switch_native_scanout_release(native);
    }
}

// After a latch: every frame no set shows goes back to its producer; with window B idle, GPU surfaces go back too, and
// slots whose allocations were freed go. A slot with nothing to give back is passed before the busy check: most of a
// pool's mappings hold no lease.
void planeReleaseIdle()
{
    planeUseHoldScanned();
    planeForgetDrain();
    const bool idle = planeWindowIdle();
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        const PlaneSlot &slot = planeSlots[k];
        const bool engine = slot.mappedAllocation != 0;
        const bool holds =
            slot.forgotten || (engine ? (slot.native && slot.native->lease) || planeUseNotes(k) : slot.iova && idle);
        if (!holds || planeSlotBusy(static_cast<int>(k)))
            continue;
        if (engine && !slot.forgotten)
            planeReleaseLease(k);
        else
            planeDropSlot(k);
    }
}
