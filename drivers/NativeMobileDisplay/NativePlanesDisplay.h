// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's hardware planes (r18-planes): included in the class body after stopDirect(). The rules, their
// sources and their tags are in NativePlanes.h; the members this uses are declared after the direct-scanout ones. Everything
// here runs under the family's work-loop gate.

#include "NativeLegacySwapQueue.h"

// The layers the screen has, as the kicks took them: each layer's surface and geometry (a layer a swap does not update keeps
// what an earlier swap gave it, as the family's effective surfaces (swapEffectiveSurfaces) do).
struct PlaneLayerRecord {
    OSObject *surface;
    NativePlanes::Layer layer;
};

PlaneLayerRecord planeLayers[NativePlanes::kLayers] = {};
PlaneSet planeTarget = {};         // the window B state the last change taken establishes (written, held or carried)
OSObject *planeShownUi = nullptr;  // the surface window A was last given
uint64_t planeRequestedAt = 0;     // frameEnds when the last window B set was written
DisplaySmmuProbe planeProbe;       // window B's SMMU proof, at the start (DisplaySmmuProbe.h's stages on window B)
int planeProbeBuffer = kNoBuffer;
uint32_t planeProbePixel = 0;
char planeProbeLine[160] = {};

uint32_t planeSlotIova(int k) const
{
    return (uint32_t)(kPlaneSlot0 + (uint64_t)k * kPlaneSlotSpan);
}

PlaneSet &planeSetOf(int set)
{
    return planeSets[set >= 0 ? set : (int)kPlaneSlots];
}

bool planesReady() const
{
    return __atomic_load_n(&planesOn, __ATOMIC_ACQUIRE);
}

// --- A swap's layers ---------------------------------------------------------------------------------------------------
static NativePlanes::Surface planeSurfaceOf(OSObject *surface)
{
    NativePlanes::Surface f;
    f.format = native_legacy_surface_format(surface);
    f.planes = native_legacy_surface_planes(surface);
    f.width = (uint32_t)native_legacy_surface_width(surface);
    f.height = (uint32_t)native_legacy_surface_height(surface);
    f.allocation = native_legacy_surface_allocation(surface);
    IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
    f.descriptorBytes = memory && memory->metaCast("IOMemoryDescriptor") ? memory->getLength() : 0;
    auto narrow = [](uint64_t v) {
        return v > 0xffffffffull ? 0xffffffffu : (uint32_t)v;
    };
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

static NativePlanes::Layer planeLayerOf(const unsigned char *bytes, unsigned i, OSObject *surface)
{
    NativePlanes::Layer l;
    l.surface = planeSurfaceOf(surface);
    l.kind = NativePlanes::kindOf(l.surface.format, l.surface.planes);
    l.src = NativePlanes::rectAt(bytes, swapSourceRects, i);
    l.dst = NativePlanes::rectAt(bytes, swapDestinationRects, i);
    l.transform = NativePlanes::wordAt(bytes, swapTransforms + 4 * i) & 7;
    return l;
}

// What a kicked swap does: window A's surface (and whether it is a new frame), every layer off, window B's change.
struct PlaneDecision {
    OSObject *ui;
    bool uiNew, blank, planeChange;
    PlaneSet set;
};

static bool planeSame(const PlaneSet &a, const PlaneSet &b)
{
    if (a.on != b.on)
        return false;
    if (!a.on)
        return true;
    const NativePlanes::Image &x = a.image, &y = b.image;
    return a.slot == b.slot && x.colorDepth == y.colorDepth && x.position == y.position && x.size == y.size &&
           x.prescaled == y.prescaled && x.dda == y.dda && x.lineStride == y.lineStride && x.hOffset == y.hOffset &&
           x.options == y.options && x.start[0] == y.start[0] && x.start[1] == y.start[1] && x.matrix == y.matrix &&
           x.fullRange == y.fullRange;
}

void planeDecide(const OriginalLegacySwapRequest *request, PlaneDecision &d)
{
    d = {};
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(request);
    const uint32_t updated = NativePlanes::wordAt(bytes, swapUpdated);
    // The real driver takes a swap only once every updated layer's IOSurface fence signalled (UnifiedPipeline's kick waits
    // for 0x318 == 0x31c [V]). Here the GPU's work on the UI is ordered by the fence-only present, and the decoder's frames
    // are in DRAM before Core Animation has them; a mismatch is only counted [I: no IOSurface fence here is ever pending].
    if (stats && ((updated ^ NativePlanes::wordAt(bytes, swapReady)) & NativePlanes::kLayerBits))
        ++stPlaneUnready;
    OSObject *effective[NativePlanes::kLayers];
    memcpy(effective, bytes + swapEffectiveSurfaces, sizeof(effective));
    uint32_t width = 0, height = 0;
    getDisplayDimensions(&width, &height);
    bool present[NativePlanes::kLayers] = {};
    NativePlanes::Layer layers[NativePlanes::kLayers];
    for (unsigned i = 0; i < NativePlanes::kLayers; ++i) {
        PlaneLayerRecord &r = planeLayers[i];
        OSObject *e = effective[i] && effective[i]->metaCast("IOSurface") ? effective[i] : nullptr;
        if (updated & (1u << i)) {
            r.surface = e;
            if (e)
                r.layer = planeLayerOf(bytes, i, e);
        } else if (e != r.surface) {
            // The family's inherited surface is not the one recorded (a swap retired unshown before its kick): a full-frame
            // BGRA one is the UI as it is; anything else cannot be placed and is not shown.
            r.surface = nullptr;
            if (e) {
                NativePlanes::Layer l;
                l.surface = planeSurfaceOf(e);
                l.kind = NativePlanes::kindOf(l.surface.format, l.surface.planes);
                l.src = l.dst = {0, 0, (int32_t)width, (int32_t)height};
                if (NativePlanes::isUi(l, width, height)) {
                    r.surface = e;
                    r.layer = l;
                } else if (stats)
                    ++stPlaneDropped;
            }
        }
        present[i] = r.surface != nullptr;
        layers[i] = r.layer;
    }
    const NativePlanes::Roles roles = NativePlanes::rolesOf(present, layers, width, height);
    if (stats)
        stPlaneDropped += roles.extra;
    bool any = false;
    for (bool p : present)
        any = any || p;
    d.blank = !any;
    d.ui = roles.ui >= 0 ? planeLayers[roles.ui].surface : nullptr;
    d.uiNew = d.blank || (d.ui && ((updated & (1u << roles.ui)) || d.ui != planeShownUi));
    if (any && roles.ui < 0 && stats)
        ++stPlaneDropped;  // no UI layer: window A keeps the last one
    PlaneSet want = {};
    if (roles.plane >= 0) {
        NativePlanes::Image image;
        const PlaneLayerRecord &r = planeLayers[roles.plane];
        if (!planesOn) {
            if (stats)
                ++stPlaneDropped;
        } else if (NativePlanes::imageOf(r.layer, width, height, image) && image.visible) {
            const int slot =
                planeSlotFor(native_legacy_surface_descriptor(r.surface), image.readLow, image.readHigh, !image.yuv);
            if (slot >= 0)
                want = {true, slot, image, 0};
            else if (stats)
                ++stPlaneDropped;
        }
    }
    d.planeChange = !planeSame(want, planeTarget);
    d.set = want;
}

// --- IOVA slots ---------------------------------------------------------------------------------------------------------
bool planeSlotBusy(int k) const
{
    if (planeLatch.busy(k) || (planeTarget.on && planeTarget.slot == k) ||
        (planeNextSet && planeNext.on && planeNext.slot == k) || (planeHeldSet && planeHeld.on && planeHeld.slot == k))
        return true;
    for (unsigned e = 0; e < TegraScanoutRing::kMaxEntries; ++e)
        if (entryPlaneSet[e] && entryPlane[e].on && entryPlane[e].slot == k)
            return true;
    return false;
}

// The slot holding [low, high) of memory (mapped now into the least recently used slot no set uses, if not yet): -1 when
// every slot is in use or the pages cannot be had. A surface the CPU may have written (RGB: an image decoded in software)
// is cleaned to DRAM once when it is mapped: DC A does not snoop the CPU's caches. Video comes from VIC (or a CPU copy that
// cleans) already in DRAM [V: r17-video TegraVICPresent].
int planeSlotFor(IOMemoryDescriptor *memory, uint64_t low, uint64_t high, bool clean)
{
    if (!memory || !memory->metaCast("IOMemoryDescriptor") || high <= low || high - low > kPlaneSlotSpan)
        return -1;
    int victim = -1;
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        if (planeSlots[k].memory == memory && planeSlots[k].low == low && planeSlots[k].high == high) {
            planeSlots[k].used = ++planeSlotUses;
            return (int)k;
        }
        if (!planeSlotBusy((int)k) && (victim < 0 || planeSlots[k].used < planeSlots[victim].used))
            victim = (int)k;
    }
    if (victim < 0)
        return -1;
    planeDropSlot((unsigned)victim);
    if (memory->prepare(kIODirectionOut) != kIOReturnSuccess)
        return -1;
    const uint64_t end =
        high < ((memory->getLength() + 4095) & ~4095ull) ? high : ((memory->getLength() + 4095) & ~4095ull);
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
        probeDomain.unmap(iova, (uint32_t)((at - low) / 4096));
        cleanDomain();
        memory->complete(kIODirectionOut);
        return -1;
    }
    if (clean) {
        if (IOMemoryMap *view = memory->map(kIOMapAnywhere | kIOMapReadOnly)) {
            if (view->getVirtualAddress() && view->getLength() >= end)
                cleanInvalidate(reinterpret_cast<const void *>(view->getVirtualAddress() + low), end - low);
            view->release();
        }
    }
    memory->retain();
    planeSlots[victim] = {memory, low, end, ++planeSlotUses};
    cleanDomain();
    if (stats)
        ++stPlaneMaps;
    return victim;
}

void planeDropSlot(unsigned k)
{
    PlaneSlot &slot = planeSlots[k];
    if (slot.memory) {
        probeDomain.unmap(planeSlotIova((int)k), (uint32_t)((slot.high - slot.low) / 4096));
        slot.memory->complete(kIODirectionOut);
        slot.memory->release();
        cleanDomain();
    }
    slot = {};
}

// Window B is off and nothing about it is pending: the surfaces it showed go back to their owners.
void planeReleaseIdle()
{
    if (planeLatch.active != NativePlanes::PlaneLatch::kOff || planeLatch.requested() || planeTarget.on || planeHeldSet)
        return;
    for (unsigned k = 0; k < kPlaneSlots; ++k)
        if (!planeSlotBusy((int)k))
            planeDropSlot(k);
}

// --- Window B's registers ------------------------------------------------------------------------------------------------
// Window B is (or may be) scanning: the live registers (CSC, filters) must not change now.
bool planeOnScreen() const
{
    return planeLatch.active >= 0 || planeLatch.armed >= 0 || planeLatch.superseded >= 0;
}

void planeWriteCsc(const NativePlanes::Image &m)
{
    const uint32_t *k = NativePlanes::kCsc[m.matrix][m.fullRange ? 1 : 0];
    for (unsigned i = 0; i < 8; ++i)
        dc[kWincBBase + NativePlanes::kCscYof + i] = k[i];
    planeCsc[0] = (int)m.matrix;
    planeCsc[1] = m.fullRange ? 1 : 0;
}

// Once while window B is off (the filter taps are live registers): the TRM's taps, frame-boundary latching for B and C,
// window B's underflow latched (read at frame ends, never routed to the line).
void planeStartRegisters()
{
    for (unsigned i = 0; i < 16; ++i) {
        dc[kWincBBase + NativePlanes::kHFilter0 + i] = NativePlanes::kHFilterTaps[i];
        dc[kWincBBase + NativePlanes::kVFilter0 + i] = NativePlanes::vFilterTap(i);
    }
    planeFiltersWritten = true;
    dc[kDcRegActControl] = dc[kDcRegActControl] & ~(kWinBActHcntrSel | kWinCActHcntrSel);
    // Every later write of hekate's REG_ACT_CONTROL keeps windows B and C on frame-boundary latching.
    pitchActControl &= ~(kWinBActHcntrSel | kWinCActHcntrSel);
    dc[kDcIntStatus] = kWinBUnderflowInt;
    dc[kDcIntEnable] = dc[kDcIntEnable] | kWinBUnderflowInt;
}

void planeWriteRegisters(const PlaneSet &p)
{
    using namespace NativePlanes;
    volatile uint32_t *win = dc + kWinBBase, *buf = dc + kWinbufBBase;
    if (!p.on) {
        win[kOptions] = 0;
        return;
    }
    const Image &m = p.image;
    // A new matrix while window B is off goes in now; while it scans, at the frame end that shows the new set taken (the
    // CSC is not shadowed: a write now would recolour the frame being scanned) [I: the action runs in the vertical blank].
    if (m.yuv && !planeOnScreen() && (planeCsc[0] != (int)m.matrix || planeCsc[1] != (m.fullRange ? 1 : 0)))
        planeWriteCsc(m);
    if (!planeFiltersWritten && !planeOnScreen())
        planeStartRegisters();
    win[kColorDepth] = m.colorDepth;
    win[kByteSwap] = 0;
    win[kPosition] = m.position;
    win[kSize] = m.size;
    win[kPrescaled] = m.prescaled;
    win[kHInitialDda] = m.hInitial;
    win[kVInitialDda] = m.vInitial;
    win[kDdaIncrement] = m.dda;
    win[kLineStride] = m.lineStride;
    win[kBufStride] = 0;
    win[kUvBufStride] = 0;
    const uint64_t base = (uint64_t)planeSlotIova(p.slot) - planeSlots[p.slot].low;
    buf[kStartAddr] = (uint32_t)(base + m.start[0]);
    buf[kStartAddrHi] = 0;
    buf[kStartAddrU] = m.yuv ? (uint32_t)(base + m.start[1]) : 0;
    buf[kStartAddrHiU] = 0;
    buf[kHOffset] = m.hOffset;
    buf[kVOffset] = 0;
    buf[kSurfaceKind] = 0;
    buf[kCdeControl] = 0;
    win[kBlendLayer] = kBlendBypass | 2;  // opaque, behind window A (depth 0)
    win[kBlendMatch] = 0;
    win[kBlendNomatch] = 0;
    win[kOptions] = m.options;
}

// Window A blends over window B (premultiplied, in front) while B shows something; opaque as hekate left it otherwise.
void planeBlendA(bool over, uint32_t &update, uint32_t &request)
{
    if (over == planeBlending)
        return;
    volatile uint32_t *a = dc + kWinABase;
    if (over) {
        a[NativePlanes::kBlendLayer] = NativePlanes::kBlendPremultiplied;
        a[NativePlanes::kBlendMatch] = NativePlanes::kBlendOver;
        a[NativePlanes::kBlendNomatch] = NativePlanes::kBlendOver;
    } else
        for (unsigned i = 0; i < 3; ++i)
            a[NativePlanes::kBlendLayer + i] = planeBlendSaved[i];
    planeBlending = over;
    update |= kGeneralUpdate | kWinAUpdate;
    request |= kGeneralActReq | kWinAActReq;
}

// Window B's set p into its assembly registers, with the request bits for the caller's one STATE_CONTROL write; or, while
// which of the last two sets the boundary took is open, held for the frame end that settles it (newest wins).
bool planeStage(const PlaneSet &p, uint32_t &update, uint32_t &request)
{
    planeTarget = p;
    if (!planeLatch.mayWrite()) {
        planeHeld = p;
        planeHeldSet = true;
        return false;
    }
    planeWriteRegisters(p);
    planeBlendA(p.on, update, request);
    update |= kGeneralUpdate | kWinBUpdate;
    request |= kGeneralActReq | kWinBActReq;
    const int set = p.on ? p.slot : NativePlanes::PlaneLatch::kOff;
    planeLatch.write(set);
    planeSetOf(set) = p;
    planeRequestedAt = frameEnds;
    if (flipTimer)
        flipTimer->setTimeoutMS(kFlipDeadlineMS);
    if (stats)
        ++stPlaneFlips;
    return true;
}

// A change of window B alone, from the next frame boundary (no window A frame carries it).
void planeFlipOnly(const PlaneSet &p)
{
    uint32_t update = 0, request = 0;
    if (!planeStage(p, update, request))
        return;
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
}

// A swap that changes window B and not window A (a video frame under a still UI): no GPU work at all. Its set is written now,
// or carried by the window A frame still held for a flip (which must reach the screen first), and the swap retires at the
// latch that shows it. A swap that changes nothing retires with whatever is still out, or now.
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
        if (ring.held != kNoBuffer) {
            entryPlane[ring.held] = p;
            entryPlaneSet[ring.held] = true;
            entrySeq[ring.held] = seq;
            planeTarget = p;
        } else
            planeFlipOnly(p);
    } else if (latchRetire) {
        if (planeLatch.requested())
            planeSetOf(planeLatch.armed).seq = seq;
        else if (planeHeldSet)
            planeHeld.seq = seq;
        else if (ring.held != kNoBuffer)
            entrySeq[ring.held] = seq;
        else if (ring.pending != kNoBuffer)
            entrySeq[ring.pending] = seq;
        else
            retireThrough(seq, mach_absolute_time());
    }
    if (!latchRetire) {
        transferred = true;
        finishTransfer(request, true);
        return;
    }
    rekick = true;  // the next swap may already be queued behind this one: take it in the same kick
}

// Window B's active START_ADDR as a set (its slot), kOff when it is not enabled, kUnknown when it names no slot.
int planeScannedSet()
{
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    const uint32_t options = dc[kWinBBase + NativePlanes::kOptions],
                   start = dc[kWinbufBBase + NativePlanes::kStartAddr];
    dc[kDcStateAccess] = access;
    if (!(options & kWinEnable))
        return NativePlanes::PlaneLatch::kOff;
    if (start < kPlaneSlot0 || start >= kPlaneSlot0 + (uint64_t)kPlaneSlots * kPlaneSlotSpan)
        return NativePlanes::PlaneLatch::kUnknown;
    return (int)((start - kPlaneSlot0) / kPlaneSlotSpan);
}

// Every DC A frame end, under the gate.
void planeFrameEnd()
{
    if (planeProbeTick() || !planesAttached || hwSuspended || scanoutStopped || gPanicRestore.panicking)
        return;
    if (planeLatch.requested()) {
        const bool out = dc[kDcStateControl] & kWinBActReq;
        if (planeLatch.latched(out, planeScannedSet())) {
            if (stats)
                ++stPlaneLatched;
            const PlaneSet now = planeSetOf(planeLatch.active);
            if (now.on && now.image.yuv &&
                (planeCsc[0] != (int)now.image.matrix || planeCsc[1] != (now.image.fullRange ? 1 : 0)))
                planeWriteCsc(now.image);
            retireThrough(now.seq, __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE));
            if (planeHeldSet && planeLatch.mayWrite()) {
                planeHeldSet = false;
                planeFlipOnly(planeHeld);
            }
            planeReleaseIdle();
            if (kickDeferred) {
                kickDeferred = false;
                kickSwapQueue();
            }
        } else if (out && frameEnds - planeRequestedAt >= kLatchFrames)
            planesOff("window B did not take its registers in 4 frames", true);
    }
    const uint32_t underflow = dc[kDcIntStatus] & kWinBUnderflowInt;
    if (!underflow) {
        planeUnderflowRun = 0;
        return;
    }
    dc[kDcIntStatus] = underflow;
    if (stats)
        ++stPlaneUnderflows;
    if (planeLatch.active >= 0 && ++planeUnderflowRun >= kPlaneUnderflowFrames)
        planesOff("window B underflowed 3 frames in a row", false);
}

// One line, window B off from the next boundary and for the rest of the boot: detached layers are not shown from here
// (counted as dropped; Core Animation, which read the limits at its start, may still send them: never refused). `stuck`:
// window B takes no registers at all; the off set is written once more and nothing about it is followed from here (its
// surfaces stay mapped until the display stops, since window B may still be reading the last set it took).
void planesOff(const char *why, bool stuck)
{
    if (!planesOn)
        return;
    if (why)
        IOLog("SwitchDisplay: planes off: %s; detached layers are not shown for the rest of this boot\n", why);
    __atomic_store_n(&planesOn, false, __ATOMIC_RELEASE);
    planeHeldSet = planeNextSet = false;
    for (bool &set : entryPlaneSet)
        set = false;
    if (hwSuspended) {
        planeTarget = {};
        planeLatch.settle();
        planeLatch.active = NativePlanes::PlaneLatch::kOff;
        return;
    }
    const PlaneSet off = {};
    planeLatch.superseded = NativePlanes::PlaneLatch::kNone;  // let the off set be written at once
    planeFlipOnly(off);
    if (stuck) {
        planeLatch = NativePlanes::PlaneLatch();
        planeTarget = {};  // nothing is requested now: planeFrameEnd and the flip deadline leave window B alone
    }
}

// The display stops: window B off, every slot given back (the caller detaches the domain).
void stopPlanes()
{
    if (planeProbe.active()) {
        PlaneProbeIo io = {this};
        planeProbe.perform(planeProbe.fail("the display stopped"), io);
    }
    if (dc && planesAttached && !hwSuspended) {
        dc[kWinBBase + NativePlanes::kOptions] = 0;
        for (unsigned i = 0; i < 3; ++i)
            dc[kWinABase + NativePlanes::kBlendLayer + i] = planeBlendSaved[i];
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate | kWinBUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq | kWinBActReq;
    }
    gPanicRestore.planes = false;
    __atomic_store_n(&planesOn, false, __ATOMIC_RELEASE);
    planeLatch = NativePlanes::PlaneLatch();
    planeTarget = {};
    planeHeldSet = planeNextSet = planeBlending = false;
    for (bool &set : entryPlaneSet)
        set = false;
    for (unsigned k = 0; k < kPlaneSlots; ++k)
        planeDropSlot(k);
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
    planeCsc[0] = planeCsc[1] = -1;
    planeFiltersWritten = false;
}

// SC7, after the cold re-init and the domain's re-attach: window B and window A's blending as they were.
void planesResume()
{
    if (!planesOn)
        return;
    planeStartRegisters();
    const PlaneSet now = planeSetOf(planeLatch.active);
    uint32_t update = kGeneralUpdate | kWinAUpdate | kWinBUpdate, request = kGeneralActReq | kWinAActReq | kWinBActReq;
    if (now.on && now.image.yuv)
        planeWriteCsc(now.image);
    planeWriteRegisters(now);
    const bool over = now.on;
    planeBlending = !over;  // force the write: the cold init put hekate's values back
    planeBlendA(over, update, request);
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
    if (planeHeldSet) {
        planeHeldSet = false;
        planeFlipOnly(planeHeld);
    }
}

// --- The plane probe (window B through the SMMU, before backboardd starts) ----------------------------------------------
struct PlaneProbeIo {
    NativeLegacyDisplay *d;

    void tweakPixel()
    {
        d->planeProbeWritePixel(d->planeProbePixel ^ 0x00010101u);
    }

    void restorePixel()
    {
        d->planeProbeWritePixel(d->planeProbePixel);
    }

    bool attach()
    {
        return d->planeDomainAttach();
    }

    void scan(bool iova)
    {
        d->planeProbeScan(iova);
    }

    void detach()
    {
        d->planeProbeDetach();
    }

    void crcOff()
    {
        d->planeProbeOff();
    }
};

void planeProbeWritePixel(uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(surfaceMapping(planeProbeBuffer)->getVirtualAddress()) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// True while the probe owns window B. It runs once, about half a second after the start, before the first client frame:
// window B covers the screen (opaque, in front) with a copy of what window A shows (the console tape's window or the boot
// framebuffer), so the CRC depends on window B alone and the console seems to pause for about 20 frames.
bool planeProbeTick()
{
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
    if (!doubleBuffer || !probeTables || !SWITCH_PORTRAIT_NATIVE || hwSuspended || scanoutStopped ||
        gPanicRestore.panicking || probe.active() || frameEnds < 30)
        return false;
    planeProbeRan = true;
    planeProbeStart();
    return planeProbe.active();
}

void planeProbeStart()
{
    checkPitchWindow();  // window A is hekate's pitch window, on the boot framebuffer or the console tape
    planeProbeBuffer = ring.buffers >= 3 ? 2 : 1;
    const uint32_t start = windowActive(kWinbufStartAddr), stride = windowActive(kWinLineStride) & 0xffff;
    const unsigned char *from = nullptr;
    if (tapeAttached && tapeBuffer && tapeBuffer->getBytesNoCopy() && start >= tapeAddress &&
        (uint64_t)(start - tapeAddress) + kTapeWindow <= kTapeBytes && stride == kTapeStride) {
        // The console wrote the tape through its write-combined view; the cacheable alias, invalidated, reads DRAM.
        from = static_cast<const unsigned char *>(tapeBuffer->getBytesNoCopy()) + (start - tapeAddress);
        cleanInvalidate(from, kTapeWindow);
    } else if (start == surfaceAddress(0) && stride == 2880)
        from = reinterpret_cast<const unsigned char *>(bootMapping->getVirtualAddress());
    volatile uint64_t *to =
        reinterpret_cast<volatile uint64_t *>(surfaceMapping(planeProbeBuffer)->getVirtualAddress());
    for (uint32_t row = 0; row < 1280; ++row)
        for (uint32_t w = 0; w < 2880 / 8; ++w) {
            uint64_t v = 0xff000000ff000000ull;
            if (from)
                memcpy(&v, from + (uint64_t)row * stride + w * 8, 8);
            to[(uint64_t)row * 360 + w] = v;
        }
    __asm__ volatile("dsb sy" ::: "memory");
    planeProbePixel = *reinterpret_cast<volatile uint32_t *>(to);
    for (unsigned i = 0; i < 3; ++i)
        planeBlendSaved[i] = dc[kWinABase + NativePlanes::kBlendLayer + i];
    for (unsigned i = 0; i < 3; ++i)
        gPanicRestore.blend[i] = planeBlendSaved[i];
    chainPanicHook();
    gPanicRestore.planes = true;
    planeStartRegisters();
    volatile uint32_t *win = dc + kWinBBase, *buf = dc + kWinbufBBase;
    win[NativePlanes::kColorDepth] = windowActive(kWinColorDepth);
    win[NativePlanes::kByteSwap] = 0;
    win[NativePlanes::kPosition] = 0;
    win[NativePlanes::kSize] = kPitchSize;
    win[NativePlanes::kPrescaled] = kPitchPrescaled;
    win[NativePlanes::kHInitialDda] = 0;
    win[NativePlanes::kVInitialDda] = 0;
    win[NativePlanes::kDdaIncrement] = kUnitDda;
    win[NativePlanes::kLineStride] = 2880;
    win[NativePlanes::kBufStride] = 0;
    win[NativePlanes::kUvBufStride] = 0;
    buf[NativePlanes::kStartAddr] = surfaceAddress(planeProbeBuffer);
    buf[NativePlanes::kStartAddrHi] = 0;
    buf[NativePlanes::kHOffset] = 0;
    buf[NativePlanes::kVOffset] = 0;
    buf[NativePlanes::kSurfaceKind] = 0;
    buf[NativePlanes::kCdeControl] = 0;
    win[NativePlanes::kBlendLayer] = NativePlanes::kBlendBypass;  // depth 0: in front of window A (hekate's 255)
    win[NativePlanes::kOptions] = kWinEnable;
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
    if (!directAttached && !planesAttached &&
        !probeDomain.init(probeTables->getBytesNoCopy(), probeTablesPa, kProbeTables)) {
        snprintf(planeProbeLine, sizeof(planeProbeLine), "the table buffer cannot hold a directory");
        return false;
    }
    bool ok = true;
    for (unsigned i = 0; i < ring.buffers; ++i)
        ok = ok && probeDomain.identity(surfaceAddress((int)i), kNativeBlockBytes, false);
    if (ok && tapeAttached)
        ok = probeDomain.identity(tapeAddress, kTapeBytes, false);
    uint64_t pages[64];
    for (uint64_t off = 0; ok && off < kNativeBlockBytes; off += 64 * 4096) {
        uint32_t n = 0;
        for (; n < 64 && off + n * 4096ull < kNativeBlockBytes; ++n)
            pages[n] = surfaceAddress(planeProbeBuffer) + off + n * 4096ull;
        ok = probeDomain.map(planeSlotIova(0) + off, pages, n, false);
    }
    if (!ok) {
        snprintf(planeProbeLine, sizeof(planeProbeLine), "more than %u SMMU tables needed", kProbeTables);
        return false;
    }
    uintptr_t low, high;
    if (probeDomain.takeDirty(&low, &high))
        cleanInvalidate(reinterpret_cast<const void *>(low), high - low);
    if (tegra_smmu_attach(TEGRA_SMMU_ASID_DISPLAY, probeDomain.ptb(), TEGRA_SMMU_DC_ASID,
                          TEGRA_SMMU_TRANSLATION_ENABLE_0,
                          kDisplay0bTranslation | (directAttached ? kDisplay0aTranslation : 0), planeProbeLine,
                          sizeof(planeProbeLine)))
        return false;
    planesAttached = true;
    return true;
}

void planeProbeScan(bool iova)
{
    dc[kWinbufBBase + NativePlanes::kStartAddr] = iova ? planeSlotIova(0) : surfaceAddress(planeProbeBuffer);
    dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinBActReq;
}

// Passed (kFinish): the domain stays, the slot range is reserved, planes are on. Failed (kAbort, after the attach): the
// probe's mapping goes; the swgroup back to bypass unless direct scanout uses it.
void planeProbeDetach()
{
    probeDomain.unmap(planeSlotIova(0), (uint32_t)(kNativeBlockBytes / 4096));
    if (planeProbe.stage == DisplaySmmuProbe::kDone) {
        if (probeDomain.reserve(kPlaneSlot0, (uint64_t)kPlaneSlots * kPlaneSlotSpan)) {
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
    dc[kWinBBase + NativePlanes::kOptions] = 0;
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
    if (planeProbe.stage == DisplaySmmuProbe::kDone && planesOn)
        IOLog(
            "SwitchDisplay: plane probe ok: window B reads through the SMMU (ASID %u, display0b, "
            "TRANSLATION_ENABLE_0 "
            "%#x): CRC %#x by physical address, 1:1 and at IOVA %#x, %#x with one pixel changed (first CRC latched at "
            "frame end %u, %u frame ends); planes on (%u slots of %llu MiB)\n",
            (unsigned)TEGRA_SMMU_ASID_DISPLAY, tegra_smmu_read(TEGRA_SMMU_TRANSLATION_ENABLE_0), planeProbe.base,
            planeSlotIova(0), planeProbe.tweaked, planeProbe.firstLatch, planeProbe.frames, kPlaneSlots,
            (unsigned long long)(kPlaneSlotSpan >> 20));
    else
        IOLog("SwitchDisplay: plane probe failed at %s: %s%s%s (CRC base %#x again %#x pixel %#x last %#x, first "
              "latched at frame end %u of %u); planes off, Core Animation keeps compositing every layer\n",
              DisplaySmmuProbe::name(planeProbe.stage == DisplaySmmuProbe::kDone ? DisplaySmmuProbe::kDone
                                                                                 : planeProbe.failedAt),
              planeProbe.stage == DisplaySmmuProbe::kDone ? "the plane slots" : planeProbe.why,
              planeProbeLine[0] ? ": " : "", planeProbeLine, planeProbe.base, planeProbe.again, planeProbe.tweaked,
              planeProbe.seen, planeProbe.firstLatch, planeProbe.frames);
}
