// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's landscape mode (SwitchLandscape.h, SwitchLandscapeWindows.h): included in the class body after
// NativePlanesDisplay.h; everything here runs under the work-loop gate. A composition whose mode differs from the
// latched one never goes out with anything else. From the moment one is staged the kicks stop; at the first frame end
// with nothing in flight the UI's registers move to the other window and the overlays to theirs, all three windows in
// one GENERAL_ACT_REQ, so one boundary takes the UI (the frame already on screen) and the video together; the frame end
// that shows that request taken releases the kicks. The windows' CSC and filter taps are live: each is written only
// while its window scans the RGB UI unscaled, which reads neither.
// Leaving cannot wait in three cases (SC7, a stop, an overlay recovery): landscapeLeaveNow puts the portrait windows
// into the assembly registers at once, overlays off.

// A wanted change waits at most this long for a frame end with nothing in flight (the kicks are stopped, so what is
// out drains within a frame or two).
static constexpr unsigned kLandscapeQuietFrames = 120;

// The UI's window words (DC_WIN 0x700 + i, DC_WINBUF 0x800 + i), WIN_OPTIONS moved last.
static constexpr uint32_t kUiWindowWords[] = {
    native_planes::kColorDepth, native_planes::kByteSwap,    native_planes::kPosition,    native_planes::kSize,
    native_planes::kPrescaled,  native_planes::kHInitialDda, native_planes::kVInitialDda, native_planes::kDdaIncrement,
    native_planes::kLineStride, native_planes::kBufStride,   native_planes::kUvBufStride};
static constexpr uint32_t kUiBufferWords[] = {
    native_planes::kStartAddr, native_planes::kStartAddrHi, native_planes::kStartAddrU,  native_planes::kStartAddrHiU,
    native_planes::kHOffset,   native_planes::kVOffset,     native_planes::kSurfaceKind, native_planes::kCdeControl};

// The overlay requests a latch waits for: during a change of mode, all three windows'.
uint32_t landscapeRequestBits() const
{
    return landscapeState.switching ? kWinAActReq | kWinBActReq | kWinCActReq
                                    : switch_landscape::overlayRequests(landscapeOn());
}

bool landscapeHoldsKicks() const
{
    return landscapeState.wanted || landscapeState.switching;
}

// From planeStage: true when p waits for a change of mode (or for the one out to latch) instead of going out now.
bool landscapeHolds(PlaneSet &p)
{
    if (landscapeState.switching) {
        planeHeld = p;
        planeHeldSet = true;
        planeTarget = p;
        return true;
    }
    const uint64_t earlier = landscapeState.wanted ? (landscapeState.held.pendingSeq ? landscapeState.held.pendingSeq
                                                                                     : landscapeState.held.seq)
                                                   : 0;
    const uint64_t own = p.pendingSeq ? p.pendingSeq : p.seq;
    if (earlier && (!own || earlier < own))
        p.pendingSeq = earlier;
    if (p.landscape == landscapeOn()) {
        // Newest wins: an older composition (one held for a latch, restaged now) is superseded by the change.
        if (landscapeState.wanted && p.seq && p.seq < landscapeState.held.seq)
            return true;
        if (landscapeState.wanted) {
            landscapeState.wanted = false;
            landscapeState.held = {};
        }
        return false;
    }
    // A stopped scanout has no frame end to wait for, and its restart's first boundary takes whatever was written.
    if (scanoutStopped && !hwSuspended) {
        landscapeState.wanted = false;
        landscapeState.held = {};
        if (p.landscape) {
            p.landscape = false;
            p.window[0] = p.window[1] = {};
        } else {
            landscapeLeaveNow("the scanout is stopped");
        }
        return false;
    }
    if (planeHeldSet && planeHeld.seq <= p.seq) {
        const uint64_t older = planeHeld.pendingSeq ? planeHeld.pendingSeq : planeHeld.seq;
        if (older && (!p.pendingSeq || older < p.pendingSeq))
            p.pendingSeq = older;
        planeHeldSet = false;
    }
    if (!landscapeState.wanted)
        landscapeState.wantedAt = frameEnds;
    landscapeState.held = p;
    landscapeState.wanted = true;
    planeTarget = p;
    return true;
}

// What keeps this frame end from taking a change of mode, or nullptr.
const char *landscapeBusy()
{
    if (transferred || copyQueued || planeFencePending)
        return "a swap is in flight";
    if (ring.pending != kNoBuffer || ring.drawing != kNoBuffer || ring.held != kNoBuffer ||
        ring.superseded != kNoBuffer)
        return "a UI flip is out";
    if (planeLatch.requested() || planeHeldSet || planeRecovering || !mediaFlipIdle())
        return "an overlay change is out";
    if (probeRequest || probe.active() || planeProbe.active() || columnScanBusy() || windowProbeHolds())
        return "a display experiment runs";
    if (tapeAttached || tapeSettle)
        return "the console tape is on";
    if (dc[kDcStateControl] & (kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq))
        return "a request is out";
    return nullptr;
}

// Every DC A frame end, after planeFrameEnd.
void landscapeFrameEnd()
{
    if (!landscapeState.wanted || hwSuspended || scanoutStopped || gPanicRestore.panicking)
        return;
    if (const char *busy = landscapeBusy()) {
        if (frameEnds - landscapeState.wantedAt < kLandscapeQuietFrames)
            return;
        IOLog("SwitchDisplay: no quiet frame end for a landscape change in %u frames (%s): landscape off this boot\n",
              kLandscapeQuietFrames, busy);
        landscapeState.retired = true;
        const bool leave = landscapeOn();
        landscapeAbandonWant();
        if (leave)
            landscapeLeaveNow("no quiet frame end");
        if (kickDeferred) {
            kickDeferred = false;
            kickSwapQueue();
        }
        return;
    }
    PlaneSet p = landscapeState.held;
    landscapeState.wanted = false;
    landscapeState.held = {};
    if (p.landscape)
        landscapeEnter(p);
    else
        landscapeLeave(p);
}

// The wanted change will not happen: its composition retires with whatever is out, or now.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
void landscapeAbandonWant()
{
    if (!landscapeState.wanted)
        return;
    const uint64_t seq = landscapeState.held.seq;
    landscapeState.wanted = false;
    landscapeState.held = {};
    planeTarget = planeSetOf(planeLatch.active);
    if (stats)
        ++stPlaneDropped;
    retireUnshown(seq);
}

// The scanout stops (display power off): a held change resolves now, leaving at once or never entering.
void landscapeScanoutStopped()
{
    if (!landscapeState.wanted)
        return;
    if (landscapeState.held.landscape)
        landscapeAbandonWant();
    else
        landscapeLeaveNow("the scanout stopped");
}

// Under the gate: the UI's registers from window `from` onto window `to` (assembly copies), `to` in front.
void landscapeMoveUi(Window from, Window to)
{
    withdrawCompletionFlip();
    using namespace switch_landscape;
    dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    for (const uint32_t word : kUiWindowWords)
        dc[windowBase(to) + word] = static_cast<uint32_t>(dc[windowBase(from) + word]);
    for (const uint32_t word : kUiBufferWords)
        dc[bufferBase(to) + word] = static_cast<uint32_t>(dc[bufferBase(from) + word]);
    dc[windowBase(to) + native_planes::kOptions] =
        static_cast<uint32_t>(dc[windowBase(from) + native_planes::kOptions]);
}

void landscapeRequest()
{
    planeRequestedAt = frameEnds;
    landscapeState.switching = true;
    armFlipDeadline();
    if (stats)
        ++stPlaneFlips;
    scanoutMemoryBarrier();
    dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate | kWinBUpdate | kWinCUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq;
}

// p (landscape) at a quiet frame end: window A's rotated reservation first (an underflow is the risk), then the UI from A
// onto B in front, the video onto A behind it with A's CSC and taps (A still scans the UI: RGB, unscaled), C as p has it.
void landscapeEnter(PlaneSet p)
{
    mediaFlipFromGate();
    if (!planeReserveBandwidth(p)) {
        IOLog("SwitchDisplay: window A's rotated reservation could not be armed: landscape off this boot\n");
        landscapeState.retired = true;
        planeTarget = planeSetOf(planeLatch.active);
        retireUnshown(p.seq);
        if (kickDeferred) {
            kickDeferred = false;
            kickSwapQueue();
        }
        return;
    }
    PlaneSet commit = p;
    if (!commit.pendingSeq)
        commit.pendingSeq = commit.seq;
    if (planeLatch.write(commit) < 0)
        panic("SwitchDisplay: no composition slot for the landscape change at a quiet frame end");
    planeTarget = p;
    landscapeMoveUi(switch_landscape::kA, switch_landscape::kB);
    gPanicRestore.uiDepth = static_cast<uint32_t>(dc[kWinBBase + native_planes::kColorDepth]);
    const SwitchMMIOWords b = dc + kWinBBase;
    b[native_planes::kBlendLayer] = switch_landscape::kUiOnB.layer;
    b[native_planes::kBlendMatch] = switch_landscape::kUiOnB.match;
    b[native_planes::kBlendNomatch] = switch_landscape::kUiOnB.nomatch;
    planeWriteWindowRegisters(switch_landscape::kA, p.window[0], false);
    planeWriteWindowRegisters(switch_landscape::kC, p.window[1], planeWindowLive(switch_landscape::kC));
    planeBlending = true;
    chainPanicHook();
    gPanicRestore.landscape = true;
    landscapeRequest();
}

// p (portrait) at a quiet frame end: the UI from B back onto A (in front of the overlays when p has any, as hekate left
// it when not), the overlays onto B and C with B's CSC and taps (B still scans the UI).
void landscapeLeave(PlaneSet p)
{
    mediaFlipFromGate();
    if (!planeReserveBandwidth(p))
        planeDropOverlays(p, 3);
    PlaneSet commit = p;
    if (!commit.pendingSeq)
        commit.pendingSeq = commit.seq;
    if (planeLatch.write(commit) < 0)
        panic("SwitchDisplay: no composition slot for the portrait change at a quiet frame end");
    planeTarget = p;
    landscapeMoveUi(switch_landscape::kB, switch_landscape::kA);
    uint32_t update = 0, request = 0;
    planeBlending = !p.on();
    planeBlendA(p.on(), update, request);
    planeWriteWindowRegisters(switch_landscape::kB, p.window[0], false);
    planeWriteWindowRegisters(switch_landscape::kC, p.window[1], planeWindowLive(switch_landscape::kC));
    landscapeRequest();
}

// planeFrameEnd, at the latch of a change of mode: the UI window now scans the frame the ring last showed. A stopped
// scanout settles it unseen (settleStopped).
void landscapeLatched(bool seen = true)
{
    if (!landscapeState.switching)
        return;
    landscapeState.switching = false;
    const uint32_t start = seen ? uiActive(kWinbufStartAddr) : expectedStart();
    const uint32_t options = seen ? uiActive(kWinOptions) : kWinEnable;
    if (start != expectedStart() || !(options & kWinEnable)) {
        panic("SwitchDisplay: window %c did not take the UI at the %s change: START %#x OPTIONS %#x, expected %#x",
              uiName(), landscapeOn() ? "landscape" : "portrait", start, options, expectedStart());
    }
    if (!landscapeOn())
        gPanicRestore.landscape = false;
#if SWITCH_FRAME_TIMELINE
    IOLog("SwitchDisplay: %s: UI on window %c (START %#x)\n",
          landscapeOn() ? "landscape on, the video column-scanned on window A" : "landscape off", uiName(), start);
#endif
}

// No frame end may be waited for: the portrait windows into the assembly registers now (the UI from B onto A, B and C
// off, A opaque), every composition in the latch the empty portrait one (its sequence kept), a wanted change dropped.
void landscapeLeaveNow(const char *why)
{
    mediaFlipFromGate();
    landscapeAbandonWant();
    const int ids[] = {planeLatch.active, planeLatch.armed, planeLatch.superseded};
    bool uiOnB = false;
    for (const int id : ids)
        uiOnB = uiOnB || (id >= 0 && planeSetOf(id).landscape);
    if (!uiOnB)
        return;
    withdrawCompletionFlip();
    IOLog("SwitchDisplay: landscape left at once: %s\n", why);
    const bool uiWrittenOnB =
        planeLatch.requested() ? planeSetOf(planeLatch.armed).landscape : planeSetOf(planeLatch.active).landscape;
    if (uiWrittenOnB)
        landscapeMoveUi(switch_landscape::kB, switch_landscape::kA);
    dc[kWinBBase + native_planes::kOptions] = 0;
    dc[kWinCBase + native_planes::kOptions] = 0;
    for (unsigned i = 0; i < 3; ++i)
        dc[kWinABase + native_planes::kBlendLayer + i] = planeBlendSaved[i];
    planeBlending = false;
    for (const int id : ids) {
        if (id < 0)
            continue;
        PlaneSet &set = planeSetOf(id);
        set.landscape = false;
        set.window[0] = set.window[1] = {};
    }
    landscapeState.switching = false;
    planeTarget = {};
    landscapeState.csc[0] = landscapeState.csc[1] = -1;
    gPanicRestore.landscape = false;
    if (!hwSuspended) {
        scanoutMemoryBarrier();
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate | kWinBUpdate | kWinCUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq;
    }
}

// The display's planes go (stopPlanes): nothing of the mode survives.
void landscapeForget()
{
    landscapeState.wanted = landscapeState.switching = false;
    landscapeState.held = {};
    landscapeState.csc[0] = landscapeState.csc[1] = -1;
    landscapeState.filtersWritten = false;
    gPanicRestore.landscape = false;
}
