// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's hardware planes (r18-planes): included in the class body after stopDirect(). The rules, their
// sources and their tags are in NativePlanes.h; the members this uses are declared after the direct-scanout ones. Everything
// here runs under the family's work-loop gate.

#include "NativeLegacySwapQueue.h"

// The layers the screen has, as the kicks took them: each layer's surface and geometry (a layer a swap does not update keeps
// what an earlier swap gave it, as the family's effective surfaces (swapEffectiveSurfaces) do).
struct PlaneLayerRecord {
    OSObject *surface;
    native_planes::Layer layer;
};

PlaneLayerRecord planeLayers[native_planes::kLayers] = {};
PlaneSet planeTarget = {};         // the window B state the last change taken establishes (written, held or carried)
OSObject *planeShownUi = nullptr;  // the surface window A was last given
uint64_t planeRequestedAt = 0;     // frameEnds when the last window B set was written
DisplaySmmuProbe planeProbe;       // window B's SMMU proof, at the start (DisplaySmmuProbe.h's stages on window B)
int planeProbeBuffer = kNoBuffer;
uint32_t planeProbePixel = 0;
char planeProbeLine[160] = {};

static uint32_t planeSlotIova(int k)
{
    return static_cast<uint32_t>(kPlaneSlot0 + static_cast<uint64_t>(k) * kPlaneSlotSpan);
}

PlaneSet &planeSetOf(int set)
{
    return planeSets[set >= 0 ? set : static_cast<int>(PlaneCommits::kSlots)];
}

const PlaneSet &planeSetOf(int set) const
{
    return planeSets[set >= 0 ? set : static_cast<int>(PlaneCommits::kSlots)];
}

bool planesReady() const
{
    return __atomic_load_n(&planesOn, __ATOMIC_ACQUIRE) && memoryAdmissionReady();
}

// --- A swap's layers ---------------------------------------------------------------------------------------------------
static native_planes::Surface planeSurfaceOf(OSObject *surface)
{
    native_planes::Surface f;
    f.format = native_legacy_surface_format(surface);
    f.planes = native_legacy_surface_planes(surface);
    f.width = static_cast<uint32_t>(native_legacy_surface_width(surface));
    f.height = static_cast<uint32_t>(native_legacy_surface_height(surface));
    f.allocation = native_legacy_surface_allocation(surface);
    const IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): the surface's own descriptor, not retained for us
    f.descriptorBytes = memory && memory->metaCast("IOMemoryDescriptor") ? memory->getLength() : 0;
    const auto narrow = [](uint64_t v) {
        return v > 0xffffffffULL ? 0xffffffffu : static_cast<uint32_t>(v);
    };
    TegraVICSurfaceDesc color = {};
    switch_vic_surface_color(surface, &color);
    f.colorMatrix = color.colorMatrix;
    f.transfer = color.transfer;
    f.primaries = color.primaries;
    f.chromaTop = color.chromaTop;
    f.chromaBottom = color.chromaBottom;
    if (f.planes >= 2) {
        for (uint32_t p = 0; p < 2; ++p) {
            f.offset[p] = native_planes_surface_plane_offset(surface, p);
            f.stride[p] = narrow(native_planes_surface_plane_bytes_per_row(surface, p));
            f.planeHeight[p] = narrow(native_planes_surface_plane_height(surface, p));
        }
    } else {
        f.stride[0] = narrow(native_legacy_surface_stride(surface));
        f.planeHeight[0] = f.height;
    }
    return f;
}

static native_planes::Layer planeLayerOf(const unsigned char *bytes, unsigned i, OSObject *surface)
{
    native_planes::Layer l;
    l.surface = planeSurfaceOf(surface);
    l.kind = native_planes::kindOf(l.surface.format, l.surface.planes);
    l.src = native_planes::rectAt(bytes, swapSourceRects, i);
    l.dst = native_planes::rectAt(bytes, swapDestinationRects, i);
    l.transform = native_planes::wordAt(bytes, swapTransforms + 4UL * i) & 7;
    return l;
}

// What a kicked swap does: window A's surface (and whether it is a new frame), every layer off, window B's change.
struct PlaneDecision {
    OSObject *ui;
    bool uiNew, blank, planeChange;
    bool waitSlot;
    PlaneSet set;
};

#include "SwitchPlaneSelection.h"

// The GPU driver's layout of a surface it renders block-linear (by its first page): `block` filled, or null for rows.
static const tgm_layout *surfaceLayout(OSObject *surface, tgm_layout &block)
{
    IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
    if (!memory || !memory->metaCast("IOMemoryDescriptor"))
        return nullptr;
    IOByteCount length = 0;
    const addr64_t pa = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    return pa && tegra_gm20b_surface_layout(pa & ~0xfffULL, &block) == 1 ? &block : nullptr;
}

// --- IOVA slots ---------------------------------------------------------------------------------------------------------
bool planeSlotBusy(int k) const
{
    if (planeRecovering || (planeBuildingSlots & (1u << unsigned(k))) || planeConversionUses(k) ||
        planeCompositeUses(k) || planeLatch.uses(k) || planeTarget.uses(k) ||
        (landscapeState.wanted && landscapeState.held.uses(k)) || (planeFencePending && planeFencing.uses(k)) ||
        (planeNextSet && planeNext.uses(k)) || (planeHeldSet && planeHeld.uses(k)))
        return true;
    for (unsigned e = 0; e < TegraScanoutRing::kMaxEntries; ++e) {
        if (entryPlaneSet[e] && entryPlane[e].uses(k))
            return true;
    }
    return false;
}

// The slot holding [low, high) of the surface's memory (mapped now into the least recently used slot no set uses, if not
// yet): -1 when every slot is in use or the pages cannot be had, or the layout the GPU holds the surface in is not the one
// the window's registers were made for (`tiled`: block-linear). A surface the CPU may have written (RGB: an image decoded
// in software) is cleaned to DRAM once when it is mapped: DC A does not snoop the CPU's caches. Video comes from VIC (or a
// CPU copy that cleans) already in DRAM [V: r17-video TegraVICPresent]. surface: the IOSurface the memory is, held with
// it (none for the display's own buffers).
int planeSlotFor(IOMemoryDescriptor *memory, uint64_t low, uint64_t high, bool clean, OSObject *surface = nullptr,
                 bool tiled = false)
{
    if (!memory || !memory->metaCast("IOMemoryDescriptor") || high <= low || high - low > kPlaneSlotSpan)
        return -1;
    int victim = -1;
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        if (planeSlots[k].memory == memory && planeSlots[k].low == low && planeSlots[k].high == high &&
            planeSlots[k].tiled == tiled) {
            planeSlots[k].used = ++planeSlotUses;
            return static_cast<int>(k);
        }
        if (!planeSlotBusy(static_cast<int>(k)) && (victim < 0 || planeSlots[k].used < planeSlots[victim].used))
            victim = static_cast<int>(k);
    }
    if (victim < 0)
        return -1;
    planeDropSlot(static_cast<unsigned>(victim));
    if (memory->prepare(kIODirectionOut) != kIOReturnSuccess)
        return -1;
    const uint64_t end =
        high < ((memory->getLength() + 4095) & ~4095ULL) ? high : ((memory->getLength() + 4095) & ~4095ULL);
    const uint64_t iova = planeSlotIova(victim);
    uint64_t batch[64];
    uint64_t at = low;
    bool ok = end > low;
    while (ok && at < end) {
        uint32_t n = 0;
        const uint64_t from = at;
        while (n < 64 && at < end) {
            IOByteCount length = 0;
            const uint64_t pa = memory->getPhysicalSegment(at, &length, kIOMemoryMapperNone);
            if (!pa || (pa & 4095)) {
                ok = false;
                break;
            }
            batch[n++] = pa;
            at += 4096;
        }
        ok = ok && probeDomain.map(iova + (from - low), batch, n, false);
    }
    if (!ok) {
        probeDomain.unmap(iova, static_cast<uint32_t>((at - low) / 4096));
        cleanDomain();
        memory->complete(kIODirectionOut);
        return -1;
    }
    if (clean) {
        if (IOMemoryMap *view = memory->map(kIOMapAnywhere | kIOMapReadOnly)) {
            if (view->getVirtualAddress() && view->getLength() >= end) {
                switch_dcache_clean_invalidate(reinterpret_cast<const void *>(view->getVirtualAddress() + low),
                                               end - low);
            }
            view->release();
        }
    }
    memory->retain();
    if (surface)
        surface->retain();
    planeSlots[victim] = {memory, surface, nullptr, low, end, ++planeSlotUses, tiled, planeSlots[victim].native};
    cleanDomain();
    int held = 0;
    IOByteCount length = 0;
    const addr64_t first = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    planeSlots[victim].layout = first ? tegra_gm20b_surface_hold(first & ~0xfffULL, &held) : nullptr;
    if (planeSlots[victim].layout && (held != 0) != tiled) {
        planeDropSlot(static_cast<unsigned>(victim));
        return -1;
    }
    if (stats)
        ++stPlaneMaps;
    return victim;
}

bool planeWindowIdle() const
{
    return !planeSetOf(planeLatch.active).on() && !planeLatch.requested() && !planeTarget.on() && !planeHeldSet;
}

#include "SwitchPlaneSlots.h"

// --- Window B's registers ------------------------------------------------------------------------------------------------
// Window B is (or may be) scanning: the live registers (CSC, filters) must not change now.
#include "SwitchOverlayRegisters.h"

// A swap that changes window B and not window A (a video frame under a still UI): no GPU work at all. Its set is written now,
// or carried by the window A frame still held for a flip (which must reach the screen first), and the swap retires at the
// latch that shows it. A swap that changes nothing retires with whatever is still out, or now.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
void kickPlaneOnly(const OriginalLegacySwapRequest *request, const PlaneDecision &d, uint64_t kickAt)
{
    noteTaken(request, kickAt);
    const uint64_t seq = seqOf(request);
    native_legacy_transfer_observed(kIOReturnSuccess, request, 0);
    if (d.planeChange) {
        if (stats)
            ++stPlaneOnly;
        PlaneSet p = d.set;
        p.seq = seq;
        if (native_planes::planeRoute(planeFenced(p), false, false) == native_planes::PlaneRoute::kNow) {
            planeOut(p);
        } else if (planeFence(p)) {
            transferred = true;  // the next swap waits for the fence's completion
            return;
        } else {
            planeNotShown(p);
        }
    } else {
        retireUnshown(seq);
    }
    if (!latchRetire) {
        transferred = true;
        finishTransfer(request, true);
        return;
    }
    rekick = true;  // the next swap may already be queued behind this one: take it in the same kick
}

// p to window B: from the next frame boundary, or with the window A frame still held for a flip (which must reach the screen
// first).
void planeOut(const PlaneSet &p)
{
    if (presentedByMediaFlip(p))
        return;
    if (ring.held != kNoBuffer) {
        entryPlane[ring.held] = p;
        entryPlaneSet[ring.held] = true;
        entrySeq[ring.held] = p.seq;
        planeTarget = p;
    } else {
        planeFlipOnly(p);
    }
}

// Retiring at the latch, a taken swap (take sequence seq) whose frame no window A flip carries: it retires with whatever is
// still out (the window B set written or held, the window A frame held or asked for), or now.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
void retireUnshown(uint64_t seq)
{
    if (!latchRetire)
        return;
    if (planeRecovering) {
        planeRecoveryTarget.seq = seq;
    } else if (planeLatch.requested()) {
        planeSetOf(planeLatch.armed).seq = seq;
    } else if (planeHeldSet) {
        planeHeld.seq = seq;
    } else if (ring.held != kNoBuffer) {
        entrySeq[ring.held] = seq;
    } else if (ring.pending != kNoBuffer) {
        entrySeq[ring.pending] = seq;
    } else {
        retireThrough(seq, mach_absolute_time());
    }
}

// Window B's active START_ADDR as a set (its slot), kOff when it is not enabled, kUnknown when it names no slot.
#include "SwitchCompositionRegisters.h"
#include "SwitchOverlayRecovery.h"

// Every DC A frame end, under the gate.
void planeFrameEnd()
{
    if (planeProbeTick() || !planesAttached || planesAbandoned || hwSuspended || scanoutStopped ||
        gPanicRestore.panicking)
        return;
    if (planeRecoveryFrameEnd())
        return;
    if (planeLatchRequested()) {
        if (planeLatchedAtFrameEnd()) {
            if (stats)
                ++stPlaneLatched;
            landscapeRunLatched(true, landscapeState.switching);
            landscapeLatched();
            const PlaneSet now = planeSetOf(planeLatch.active);
            planeDeliver(now);
            retireThrough(now.seq, __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE));
            if (planeHeldSet && planeLatch.mayWrite()) {
                planeHeldSet = false;
                planeFlipOnly(planeHeld);
            }
            planeReleaseIdle();
            planeRetireBandwidth();
            if (kickDeferred) {
                kickDeferred = false;
                kickSwapQueue();
            }
        } else if (frameEnds - planeRequestedAt >= kLatchFrames) {
            beginPlaneRecovery("overlay registers did not latch in 4 frames");
            return;
        }
    }
    const uint32_t underflow = dc[kDcIntStatus] & switch_landscape::overlayUnderflows(landscapeOn());
    landscapeRunUnderflow(underflow);
    if (!underflow) {
        planeUnderflowRun = 0;
        return;
    }
    dc[kDcIntStatus] = underflow;
    noteLatchedUnderflow(underflow);
    if (stats)
        ++stPlaneUnderflows;
    if (planeSetOf(planeLatch.active).on() && ++planeUnderflowRun >= kPlaneUnderflowFrames)
        beginPlaneRecovery("overlay underflowed 3 frames in a row");
}

// The display stops: window B off, every slot given back (the caller detaches the domain).
void stopPlanes()
{
    mediaFlipFromGate();
    if (planeProbe.active()) {
        PlaneProbeIo io = {this};
        planeProbe.perform(planeProbe.fail("the display stopped"), io);
    }
    if (dc && planesAttached && !hwSuspended)
        landscapeLeaveNow("the display stopped");
    landscapeForget();
    if (dc && planesAttached && !hwSuspended) {
        dc[kWinBBase + native_planes::kOptions] = 0;
        dc[kWinCBase + native_planes::kOptions] = 0;
        for (unsigned i = 0; i < 3; ++i)
            dc[kWinABase + native_planes::kBlendLayer + i] = planeBlendSaved[i];
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate | kWinBUpdate | kWinCUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq;
    }
    gPanicRestore.planes = false;
    __atomic_store_n(&planesOn, false, __ATOMIC_RELEASE);
    planeLatch.reset();
    planeRecovering = false;
    planeRecoveryTarget = {};
    planeTarget = {};
    planeHeldSet = planeNextSet = planeBlending = false;
    for (bool &set : entryPlaneSet)
        set = false;
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        planeDropSlot(k);
        if (planeSlots[k].native) {
            IODelete(planeSlots[k].native, SwitchNativeScanout, 1);
            planeSlots[k].native = nullptr;
        }
    }
    convertedStop();
}

// SC7, before the power-down (under the gate, registers still clocked): a running probe is undone; the last request is what
// window B gets back; the live registers are lost with the power.
void planesQuiesce()
{
    if (planeProbe.active()) {
        PlaneProbeIo io = {this};
        planeProbe.perform(planeProbe.fail("the display suspended (SC7)"), io);
        planeProbeFinish();
    }
    planeLatch.settle();
    landscapeLeaveNow("the display suspended (SC7)");
    planeForgetUnshadowed();
}

#include "SwitchPlanesResume.h"

// --- The plane probe (window B through the SMMU, before backboardd starts) ----------------------------------------------
struct PlaneProbeIo {
    NativeLegacyDisplay *d;

    void tweakPixel() const
    {
        d->planeProbeWritePixel(d->planeProbePixel ^ 0x00010101u);
    }

    void restorePixel() const
    {
        d->planeProbeWritePixel(d->planeProbePixel);
    }

    bool attach() const
    {
        return d->planeDomainAttach();
    }

    void scan(bool iova) const
    {
        d->planeProbeScan(iova);
    }

    void detach() const
    {
        d->planeProbeDetach();
    }

    void crcOff() const
    {
        d->planeProbeOff();
    }
};

void planeProbeWritePixel(uint32_t value)
{
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): surfaceMapping lends the display's own mapping
    *reinterpret_cast<volatile uint32_t *>(surfaceMapping(planeProbeBuffer)->getVirtualAddress()) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// True while the probe owns window B. It runs once, about half a second after the start, before the first client frame:
// window B covers the screen (opaque, in front) with a copy of what window A shows (the console tape's window or the boot
// framebuffer), so the CRC depends on window B alone and the console seems to pause for about 20 frames.
bool planeProbeTick()
{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    if (!scanoutQualification)
        return false;
    if (planeProbe.active()) {
        const uint32_t underflow = dc[kDcIntStatus] & kWinBUnderflowInt;
        if (underflow)
            dc[kDcIntStatus] = underflow;
        const DisplaySmmuProbe::Action action =
            planeProbe.frameEnd(dc[kDcCrcLatched], (dc[kDcStateControl] & kWinBActReq) != 0, underflow != 0);
        PlaneProbeIo io = {this};
        if (planeProbe.perform(action, io))
            planeProbeFinish();
        return true;
    }
    if (planeProbeRan)
        return false;
    if (rotated) {
        planeProbeRan = true;
        IOLog("SwitchDisplay: plane probe not run: the first client frame came before it; planes off\n");
        return false;
    }
    if (!doubleBuffer || !probeTables.memory || !SWITCH_PORTRAIT_NATIVE || hwSuspended || scanoutStopped ||
        gPanicRestore.panicking || probe.active() || frameEnds < 30)
        return false;
    planeProbeRan = true;
    planeProbeStart();
    return planeProbe.active();
#else
    return false;
#endif
}

void planeProbeStart()
{
    checkPitchWindow();  // window A is hekate's pitch window, on the boot framebuffer or the console tape
    planeProbeBuffer = ring.buffers >= 3 ? 2 : 1;
    const uint32_t start = windowActive(kWinbufStartAddr);
    const uint32_t stride = windowActive(kWinLineStride) & 0xffff;
    const unsigned char *from = nullptr;
    if (tapeAttached && tapeBuffer.va && start >= tapeAddress &&
        static_cast<uint64_t>(start - tapeAddress) + kTapeWindow <= kTapeBytes && stride == kTapeStride) {
        // The console wrote the tape through its write-combined view; the cacheable alias, invalidated, reads DRAM.
        from = static_cast<const unsigned char *>(tapeBuffer.va) + (start - tapeAddress);
        switch_dcache_clean_invalidate(from, kTapeWindow);
    } else if (start == surfaceAddress(0) && stride == 2880) {
        from = reinterpret_cast<const unsigned char *>(bootMapping->getVirtualAddress());
    }
    volatile uint64_t *to =
        reinterpret_cast<volatile uint64_t *>(surfaceMapping(planeProbeBuffer)->getVirtualAddress());
    for (uint32_t row = 0; row < 1280; ++row) {
        for (uint32_t w = 0; w < 2880 / 8; ++w) {
            uint64_t v = 0xff000000ff000000ULL;
            if (from)
                memcpy(&v, from + static_cast<uint64_t>(row) * stride + static_cast<size_t>(w) * 8, 8);
            to[static_cast<uint64_t>(row) * 360 + w] = v;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    planeProbePixel = *reinterpret_cast<volatile uint32_t *>(to);
    for (unsigned i = 0; i < 3; ++i)
        planeBlendSaved[i] = dc[kWinABase + native_planes::kBlendLayer + i];
    for (unsigned i = 0; i < 3; ++i)
        gPanicRestore.blend[i] = planeBlendSaved[i];
    chainPanicHook();
    gPanicRestore.planes = true;
    planeStartRegisters();
    const SwitchMMIOWords win = dc + kWinBBase;
    const SwitchMMIOWords buf = dc + kWinbufBBase;
    win[native_planes::kColorDepth] = windowActive(kWinColorDepth);
    win[native_planes::kByteSwap] = 0;
    win[native_planes::kPosition] = 0;
    win[native_planes::kSize] = kPitchSize;
    win[native_planes::kPrescaled] = kPitchPrescaled;
    win[native_planes::kHInitialDda] = 0;
    win[native_planes::kVInitialDda] = 0;
    win[native_planes::kDdaIncrement] = kUnitDda;
    win[native_planes::kLineStride] = 2880;
    win[native_planes::kBufStride] = 0;
    win[native_planes::kUvBufStride] = 0;
    buf[native_planes::kStartAddr] = surfaceAddress(planeProbeBuffer);
    buf[native_planes::kStartAddrHi] = 0;
    buf[native_planes::kHOffset] = 0;
    buf[native_planes::kVOffset] = 0;
    buf[native_planes::kSurfaceKind] = 0;
    buf[native_planes::kCdeControl] = 0;
    win[native_planes::kBlendLayer] = native_planes::kBlendBypass;  // depth 0: in front of window A (hekate's 255)
    win[native_planes::kOptions] = kWinEnable;
    dc[kDcCrcControl] = kCrcOn;
    dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinBActReq;
    planeProbeLine[0] = 0;
    planeProbe.start();
}

// The domain (the direct probe's, made here when it does not exist yet): everything window A can scan 1:1 (the boot
// framebuffer, the scan surfaces, the console tape), so window A reads the same bytes should its client's translation bit
// be set; plane slot 0 onto the probe's copy; then the swgroup with window B's client (display0b) translated.
bool planeDomainAttach()
{
    if (!directAttached && !planesAttached && !probeDomain.init(probeTables.va, probeTables.pa, kProbeTables)) {
        snprintf(planeProbeLine, sizeof(planeProbeLine), "the table buffer cannot hold a directory");
        return false;
    }
    bool ok = true;
    for (unsigned i = 0; i < ring.buffers; ++i)
        ok = ok && probeDomain.identity(surfaceAddress(static_cast<int>(i)), kNativeBlockBytes, false);
    if (ok && tapeAttached)
        ok = probeDomain.identity(tapeAddress, kTapeBytes, false);
    uint64_t pages[64];
    for (uint64_t off = 0; ok && off < kNativeBlockBytes; off += 64LL * 4096) {
        uint32_t n = 0;
        for (; n < 64 && off + n * 4096ULL < kNativeBlockBytes; ++n)
            pages[n] = surfaceAddress(planeProbeBuffer) + off + n * 4096ULL;
        ok = probeDomain.map(planeSlotIova(0) + off, pages, n, false);
    }
    if (!ok) {
        snprintf(planeProbeLine, sizeof(planeProbeLine), "more than %u SMMU tables needed", kProbeTables);
        return false;
    }
    uintptr_t low = 0;
    uintptr_t high = 0;
    if (probeDomain.takeDirty(&low, &high))
        switch_dcache_clean_invalidate(reinterpret_cast<const void *>(low), high - low);
    if (tegra_smmu_attach(TEGRA_SMMU_ASID_DISPLAY, probeDomain.ptb(), TEGRA_SMMU_DC_ASID,
                          TEGRA_SMMU_TRANSLATION_ENABLE_0,
                          kDisplay0bTranslation | kDisplay0cTranslation | (directAttached ? kDisplay0aTranslation : 0),
                          planeProbeLine, sizeof(planeProbeLine)))
        return false;
    planesAttached = true;
    return true;
}

void planeProbeScan(bool iova)
{
    dc[kWinbufBBase + native_planes::kStartAddr] = iova ? planeSlotIova(0) : surfaceAddress(planeProbeBuffer);
    dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinBActReq;
}

// Passed (kFinish): the domain stays, the slot range is reserved, planes are on. Failed (kAbort, after the attach): the
// probe's mapping goes; the swgroup back to bypass unless direct scanout uses it.
void planeProbeDetach()
{
    probeDomain.unmap(planeSlotIova(0), static_cast<uint32_t>(kNativeBlockBytes / 4096));
    if (planeProbe.stage == DisplaySmmuProbe::kDone) {
        if (probeDomain.reserve(kPlaneSlot0, static_cast<uint64_t>(kPlaneSlots) * kPlaneSlotSpan)) {
            cleanDomain();
            // A swap retires when its frame is seen: the video's previous frame may still be scanned.
            latchRetire = true;
            __atomic_store_n(&planesOn, true, __ATOMIC_RELEASE);
            return;
        }
        snprintf(planeProbeLine, sizeof(planeProbeLine), "no SMMU tables for %u plane slots", kPlaneSlots);
    }
    cleanDomain();
    if (!directAttached)
        tegra_smmu_detach(TEGRA_SMMU_ASID_DISPLAY, TEGRA_SMMU_DC_ASID);
    planesAttached = false;
}

void planeProbeOff()
{
    dc[kDcCrcControl] = 0;
    dc[kWinBBase + native_planes::kOptions] = 0;
    dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinBActReq;
    if (!planesOn)
        gPanicRestore.planes = false;
}

void planeProbeFinish()
{
    if (kickDeferred) {  // a client frame came while window B covered the screen: it goes now
        kickDeferred = false;
        kickSwapQueue();
    }
    if (planeProbe.stage == DisplaySmmuProbe::kDone && planesOn) {
        IOLog(
            "SwitchDisplay: plane probe ok: window B reads through the SMMU (ASID %u, display0b, "
            "TRANSLATION_ENABLE_0 "
            "%#x): CRC %#x by physical address, 1:1 and at IOVA %#x, %#x with one pixel changed (first CRC latched at "
            "frame end %u, %u frame ends); planes on (%u slots of %llu MiB)\n",
            static_cast<unsigned>(TEGRA_SMMU_ASID_DISPLAY), tegra_smmu_read(TEGRA_SMMU_TRANSLATION_ENABLE_0),
            planeProbe.base, planeSlotIova(0), planeProbe.tweaked, planeProbe.firstLatch, planeProbe.frames,
            kPlaneSlots, static_cast<unsigned long long>(kPlaneSlotSpan >> 20));
    } else {
        IOLog("SwitchDisplay: plane probe failed at %s: %s%s%s (CRC base %#x again %#x pixel %#x last %#x, first "
              "latched at frame end %u of %u); planes off, Core Animation keeps compositing every layer\n",
              DisplaySmmuProbe::name(planeProbe.stage == DisplaySmmuProbe::kDone ? DisplaySmmuProbe::kDone
                                                                                 : planeProbe.failedAt),
              planeProbe.stage == DisplaySmmuProbe::kDone ? "the plane slots" : planeProbe.why,
              planeProbeLine[0] ? ": " : "", planeProbeLine, planeProbe.base, planeProbe.again, planeProbe.tweaked,
              planeProbe.seen, planeProbe.firstLatch, planeProbe.frames);
    }
}
