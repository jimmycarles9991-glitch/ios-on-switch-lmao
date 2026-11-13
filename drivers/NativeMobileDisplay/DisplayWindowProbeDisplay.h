// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's window probes (DisplayWindowProbe.h has the plans, DisplayWindowProbeRun.h the run), debug
// kernels only: included in the class body after NativeColumnScanDisplay.h. HW Test starts one through TegraVIC's
// kernel-test selector (switch_display_window_probe_call) and then asks for the result. The caller's thread fills the
// pictures; the run goes at DC A's frame ends under the gate with the swaps held and the frame end routed, as the
// column scan's does, and restores windows A and B by itself. Its state is static: the instance is at the 16 KiB a
// kalloc_type class may have.

#if SWITCH_FRAME_TIMELINE
static constexpr uint32_t kWindowBSelect = 1u << 5;
// Window B's DC_WIN and DC_WINBUF words the probe writes (NativePlanes.h's block-relative numbers), WIN_OPTIONS last.
static constexpr uint32_t kOverlayWindowState[] = {
    native_planes::kByteSwap,   native_planes::kColorDepth,   native_planes::kPosition,    native_planes::kSize,
    native_planes::kPrescaled,  native_planes::kHInitialDda,  native_planes::kVInitialDda, native_planes::kDdaIncrement,
    native_planes::kLineStride, native_planes::kBufStride,    native_planes::kUvBufStride, native_planes::kBlendLayer,
    native_planes::kBlendMatch, native_planes::kBlendNomatch, native_planes::kOptions};
static constexpr uint32_t kOverlayBufferState[] = {
    native_planes::kStartAddr, native_planes::kStartAddrHi, native_planes::kStartAddrU,  native_planes::kStartAddrHiU,
    native_planes::kHOffset,   native_planes::kVOffset,     native_planes::kSurfaceKind, native_planes::kCdeControl};
static constexpr size_t kWindowAWords = sizeof(kWindowAState) / sizeof(kWindowAState[0]);
static constexpr size_t kOverlayWords = sizeof(kOverlayWindowState) / sizeof(kOverlayWindowState[0]);
static constexpr size_t kOverlayBufferWords = sizeof(kOverlayBufferState) / sizeof(kOverlayBufferState[0]);
static constexpr uint32_t kWinByteSwap = 0x700 + native_planes::kByteSwap;

struct WindowProbeState {
    display_window_probe::Run run;
    display_window_probe::Plan plan;
    IOBufferMemoryDescriptor *buffer = nullptr;
    int slot = -1;
    bool claimed = false, holding = false, saved = false, reserved = false;
    const char *busyReason = nullptr;
    char why[64] = {};
    uint32_t savedA[kWindowAWords] = {}, savedAU = 0, savedAHiU = 0, savedASwap = 0;
    uint32_t savedB[kOverlayWords] = {}, savedBBuffer[kOverlayBufferWords] = {};
    uint32_t savedCrcControl = 0, savedCsc[2][8] = {}, savedHTaps[2][16] = {}, savedVTaps[2][16] = {};
    display_window_probe::ExtraRegisters extras;
};

static WindowProbeState windowProbe;

static bool windowProbeBusy()
{
    return windowProbe.claimed || windowProbe.run.busy();
}

// Swaps wait while the run has the windows.
static bool windowProbeHolds()
{
    return windowProbe.run.busy();
}

template <typename Body> void windowProbeOnLoop(Body body)
{
    getWorkLoop()->runAction(
        [](OSObject *, void *b, void *, void *, void *) -> IOReturn {
            (*static_cast<Body *>(b))();
            return kIOReturnSuccess;
        },
        this, &body);
}

// From TegraVIC's user-client thread.
IOReturn windowProbeCall(const SwitchDisplayWindowProbeRequest &request, SwitchDisplayWindowProbeReply &reply)
{
    if (!getWorkLoop())
        return kIOReturnNotReady;
    if (request.op == SWITCH_DISPLAY_WINDOW_PROBE_QUERY) {
        windowProbeOnLoop([&] {
            display_window_probe::replyOf(windowProbe.plan, windowProbe.run, reply);
        });
        return kIOReturnSuccess;
    }
    if (request.op != SWITCH_DISPLAY_WINDOW_PROBE_START)
        return kIOReturnBadArgument;
    display_window_probe::Plan plan;
    if (!display_window_probe::planOf(static_cast<display_window_probe::Probe>(request.probe), plan))
        return kIOReturnBadArgument;
    const char *why = nullptr;
    int error = 0;
    windowProbeOnLoop([&] {
        if (windowProbeBusy() || columnScanBusy()) {
            why = "a display experiment is running";
            error = EBUSY;
        } else {
            windowProbe.claimed = true;
        }
    });
    if (error) {
        windowProbeOnLoop([&] {
            display_window_probe::replyOf(windowProbe.plan, windowProbe.run, reply);
        });
    } else {
        error = windowProbePrepare(plan, why);
        windowProbeOnLoop([&] {
            if (!error)
                error = windowProbePost(plan, why);
            if (error) {  // nothing ran: the reply is this probe's, never started
                windowProbe.claimed = false;
                windowProbe.plan = plan;
                windowProbe.run = display_window_probe::Run();
            }
            display_window_probe::replyOf(windowProbe.plan, windowProbe.run, reply);
        });
    }
    reply.error = error;
    if (why)
        display_window_probe::copyName(reply.why, sizeof(reply.why), why);
    return kIOReturnSuccess;
}

// The pictures, on the caller's thread while the run is claimed. DC A does not snoop the CPU's caches.
static int windowProbePrepare(const display_window_probe::Plan &plan, const char *&why)
{
    WindowProbeState &s = windowProbe;
    OSSafeReleaseNULL(s.buffer);
    s.buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, plan.bytes, PAGE_SIZE);
    if (!s.buffer) {
        why = "no memory for the pictures";
        return ENOMEM;
    }
    auto *bytes = static_cast<uint8_t *>(s.buffer->getBytesNoCopy());
    for (unsigned i = 0; i < plan.surfaces; ++i)
        display_window_probe::fill(bytes, plan.surface[i]);
    switch_dcache_clean_invalidate(bytes, plan.bytes);
    return 0;
}

// Under the gate: what the run needs that waiting cannot bring, the pictures into a plane slot, then the swaps held.
int windowProbePost(const display_window_probe::Plan &plan, const char *&why)
{
    WindowProbeState &s = windowProbe;
    if (!SWITCH_PORTRAIT_NATIVE || !rotated || !rotationLatched) {
        why = "window A does not scan the UI's block-linear frames yet";
    } else if (!directAttached) {
        why = "window A does not read through the SMMU (direct scanout is off)";
    } else if (!planesOn) {
        why = "planes are off: no IOVA slot to map the pictures into";
    } else if (hwSuspended || scanoutStopped) {
        why = "the display is off";
    } else if ((s.slot = planeSlotFor(s.buffer, 0, plan.bytes, false)) < 0) {
        why = "no free plane slot";
    }
    if (why) {
        IOLog("SwitchDisplay: window probe %s not run: %s\n", display_window_probe::probeName(plan.probe), why);
        return EAGAIN;
    }
    s.plan = plan;
    s.busyReason = nullptr;
    mediaFlipFromGate();
    native_planes::Image heaviest;
    PlaneSet set = {};
    set.window[0].on = display_window_probe::heaviestOverlay(plan.step, plan.steps, heaviest);
    set.window[0].image = heaviest;
    s.reserved = set.window[0].on && planeReserveBandwidth(set);
    if (set.window[0].on && !s.reserved)
        IOLog("SwitchDisplay: window probe %s: window B's memory not reserved, the run goes on (an underflow is not "
              "a verdict)\n",
              display_window_probe::probeName(plan.probe));
    s.run.begin(plan.steps);
    s.holding = true;
    native_legacy_frame_end_hold();
    IOLog("SwitchDisplay: window probe %s starting: %u steps, %llu KiB of pictures at IOVA %#x\n",
          display_window_probe::probeName(plan.probe), plan.steps, static_cast<unsigned long long>(plan.bytes >> 10),
          planeSlotIova(s.slot));
    return 0;
}

uint32_t windowProbeIova(uint64_t offset) const
{
    return static_cast<uint32_t>(planeSlotIova(windowProbe.slot) + offset);
}

// Reads one window's active copy through WINDOW_HEADER (A: kWindowASelect, B: kWindowBSelect).
template <typename Read> void windowProbeActive(uint32_t select, Read read)
{
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    dc[kDcWindowHeader] = select;
    read();
    dc[kDcWindowHeader] = kWindowASelect;
    dc[kDcStateAccess] = access;
}

void windowProbeSave()
{
    WindowProbeState &s = windowProbe;
    windowProbeActive(kWindowASelect, [&] {
        for (size_t i = 0; i < kWindowAWords; ++i)
            s.savedA[i] = dc[kWindowAState[i]];
        s.savedAU = dc[kWinbufStartAddrU];
        s.savedAHiU = dc[kWinbufStartAddrHiU];
        s.savedASwap = dc[kWinByteSwap];
    });
    windowProbeActive(kWindowBSelect, [&] {
        for (size_t i = 0; i < kOverlayWords; ++i)
            s.savedB[i] = dc[0x700 + kOverlayWindowState[i]];
        for (size_t i = 0; i < kOverlayBufferWords; ++i)
            s.savedBBuffer[i] = dc[0x800 + kOverlayBufferState[i]];
    });
    s.savedCrcControl = dc[kDcCrcControl];
    const uint32_t winc[2] = {kWinc, switch_landscape::controlBase(switch_landscape::kB)};
    for (unsigned w = 0; w < 2; ++w) {
        for (unsigned i = 0; i < 8; ++i)
            s.savedCsc[w][i] = dc[winc[w] + native_planes::kCscYof + i];
        for (unsigned i = 0; i < 16; ++i) {
            s.savedHTaps[w][i] = dc[winc[w] + native_planes::kHFilter0 + i];
            s.savedVTaps[w][i] = dc[winc[w] + native_planes::kVFilter0 + i];
        }
    }
    s.saved = true;
}

// The CSC and filter taps have one copy, read live: written only while window A scans RGB and window B is off.
void windowProbeWriteUnshadowed(const uint32_t (*csc)[8], const uint32_t (*hTaps)[16], const uint32_t (*vTaps)[16])
{
    const uint32_t winc[2] = {kWinc, switch_landscape::controlBase(switch_landscape::kB)};
    for (unsigned w = 0; w < 2; ++w) {
        for (unsigned i = 0; i < 8; ++i)
            dc[winc[w] + native_planes::kCscYof + i] = csc[w][i];
        for (unsigned i = 0; i < 16; ++i) {
            dc[winc[w] + native_planes::kHFilter0 + i] = hTaps[w][i];
            dc[winc[w] + native_planes::kVFilter0 + i] = vTaps[w][i];
        }
    }
}

void windowProbeWriteProbeUnshadowed()
{
    uint32_t csc[2][8];
    uint32_t hTaps[2][16];
    uint32_t vTaps[2][16];
    for (unsigned w = 0; w < 2; ++w) {
        memcpy(csc[w], native_planes::kCsc[native_planes::kBt709][1], sizeof(csc[w]));
        memcpy(hTaps[w], native_planes::kHFilterTaps, sizeof(hTaps[w]));
        for (unsigned i = 0; i < 16; ++i)
            vTaps[w][i] = native_planes::vFilterTap(i);
    }
    windowProbeWriteUnshadowed(csc, hTaps, vTaps);
}

void windowProbeWriteA(const display_window_probe::Window &w)
{
    dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcWindowHeader] = kWindowASelect;
    if (!w.on()) {
        dc[kWinOptions] = 0;
        return;
    }
    dc[kWinColorDepth] = w.depth;
    dc[kWinByteSwap] = 0;
    dc[kWinPosition] = w.position;
    dc[kWinSize] = w.size;
    dc[kWinPrescaled] = w.prescaled;
    dc[kWinHInitialDda] = w.hInitial;
    dc[kWinVInitialDda] = w.vInitial;
    dc[kWinDdaIncrement] = w.dda;
    dc[kWinLineStride] = w.lineStride;
    dc[kWinbufSurfaceKind] = w.kind;
    dc[kWinbufStartAddr] = windowProbeIova(w.start);
    dc[kWinbufStartAddrHi] = 0;
    dc[kWinbufStartAddrU] = w.startU ? windowProbeIova(w.startU) : 0;
    dc[kWinbufStartAddrHiU] = 0;
    dc[kWinbufHOffset] = w.hOffset;
    dc[kWinbufVOffset] = w.vOffset;
    dc[kWinOptions] = w.options;
}

void windowProbeWriteB(const display_window_probe::Window &w)
{
    using namespace native_planes;
    const SwitchMMIOWords win = dc + kWinBBase;
    const SwitchMMIOWords buf = dc + kWinbufBBase;
    if (!w.on()) {
        win[kOptions] = 0;
        return;
    }
    win[kColorDepth] = w.depth;
    win[kByteSwap] = 0;
    win[kPosition] = w.position;
    win[kSize] = w.size;
    win[kPrescaled] = w.prescaled;
    win[kHInitialDda] = w.hInitial;
    win[kVInitialDda] = w.vInitial;
    win[kDdaIncrement] = w.dda;
    win[kLineStride] = w.lineStride;
    win[kBufStride] = 0;
    win[kUvBufStride] = 0;
    buf[kStartAddr] = windowProbeIova(w.start);
    buf[kStartAddrHi] = 0;
    buf[kStartAddrU] = w.startU ? windowProbeIova(w.startU) : 0;
    buf[kStartAddrHiU] = 0;
    buf[kHOffset] = w.hOffset;
    buf[kVOffset] = w.vOffset;
    buf[kSurfaceKind] = w.kind;
    buf[kCdeControl] = 0;
    win[kBlendLayer] = kBlendBypass | 1;
    win[kBlendMatch] = 0;
    win[kBlendNomatch] = 0;
    win[kOptions] = w.options;
}

void windowProbeRequest()
{
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate | kWinBUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinAActReq | kWinBActReq;
}

void windowProbeShow(const display_window_probe::Step &step)
{
    withdrawCompletionFlip();
    mediaFlipFromGate();
    windowProbeWriteA(step.a);
    windowProbeWriteB(step.b);
    windowProbe.extras.apply(dc, step.extra, step.extras);
    dc[kDcCrcControl] = kCrcOn;
    windowProbeRequest();
}

void windowProbeRestoreWindows()
{
    withdrawCompletionFlip();
    mediaFlipFromGate();
    windowProbe.extras.restore(dc);
    const WindowProbeState &s = windowProbe;
    dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcWindowHeader] = kWindowASelect;
    dc[kWinbufStartAddrU] = s.savedAU;
    dc[kWinbufStartAddrHiU] = s.savedAHiU;
    dc[kWinByteSwap] = s.savedASwap;
    for (size_t i = 0; i < kWindowAWords; ++i)
        dc[kWindowAState[i]] = s.savedA[i];
    for (size_t i = 0; i < kOverlayBufferWords; ++i)
        dc[kWinbufBBase + kOverlayBufferState[i]] = s.savedBBuffer[i];
    for (size_t i = 0; i < kOverlayWords; ++i)
        dc[kWinBBase + kOverlayWindowState[i]] = s.savedB[i];
    dc[kDcCrcControl] = s.savedCrcControl;
    windowProbeRequest();
}

// Window A's and B's active copies are the step's registers (an off window: only WIN_ENABLE clear).
bool windowProbeShows(const display_window_probe::Window &a, const display_window_probe::Window &b)
{
    bool same = true;
    const auto check = [&](uint32_t select, const display_window_probe::Window &w) {
        windowProbeActive(select, [&] {
            const uint32_t options = dc[kWinOptions];
            if (!w.on()) {
                same = same && (options & kWinEnable) == 0;
                return;
            }
            same = same && options == w.options && dc[kWinColorDepth] == w.depth && dc[kWinSize] == w.size &&
                   dc[kWinPrescaled] == w.prescaled && dc[kWinDdaIncrement] == w.dda &&
                   dc[kWinLineStride] == w.lineStride && dc[kWinbufSurfaceKind] == w.kind &&
                   dc[kWinbufStartAddr] == windowProbeIova(w.start) &&
                   dc[kWinbufStartAddrU] == (w.startU ? windowProbeIova(w.startU) : 0) &&
                   dc[kWinbufHOffset] == w.hOffset && dc[kWinbufVOffset] == w.vOffset;
        });
    };
    check(kWindowASelect, a);
    check(kWindowBSelect, b);
    return same;
}

bool windowProbeRestoredOk()
{
    const WindowProbeState &s = windowProbe;
    bool same = true;
    windowProbeActive(kWindowASelect, [&] {
        same = dc[kWinbufStartAddrU] == s.savedAU && dc[kWinbufStartAddrHiU] == s.savedAHiU &&
               dc[kWinByteSwap] == s.savedASwap;
        for (size_t i = 0; i < kWindowAWords; ++i)
            same = same && dc[kWindowAState[i]] == s.savedA[i];
    });
    windowProbeActive(kWindowBSelect, [&] {
        for (size_t i = 0; i < kOverlayWords; ++i)
            same = same && dc[0x700 + kOverlayWindowState[i]] == s.savedB[i];
        for (size_t i = 0; i < kOverlayBufferWords; ++i)
            same = same && dc[0x800 + kOverlayBufferState[i]] == s.savedBBuffer[i];
    });
    return same;
}

void windowProbePerform(display_window_probe::Run::Action action)
{
    WindowProbeState &s = windowProbe;
    switch (action) {
    case display_window_probe::Run::kNothing:
        return;
    case display_window_probe::Run::kStart:
        windowProbeSave();
        gPanicRestore.columnScanDepth = s.savedA[0];
        __asm__ volatile("dmb ish" ::: "memory");
        gPanicRestore.columnScan = true;
        windowProbeWriteProbeUnshadowed();
        windowProbeShow(s.plan.step[0]);
        return;
    case display_window_probe::Run::kShow:
        windowProbeShow(s.plan.step[s.run.step]);
        return;
    case display_window_probe::Run::kRestore:
        windowProbeRestoreWindows();
        return;
    case display_window_probe::Run::kFinish:
        windowProbeFinish();
        return;
    }
}

// At every DC A frame end, right after the column scan's: windows A's and B's underflows during the run are the run's.
void windowProbeFrameEnd()
{
    WindowProbeState &s = windowProbe;
    if (!s.run.busy())
        return;
    using Run = display_window_probe::Run;
    const Run::Stage stage = s.run.stage;
    const uint32_t underflow = s.run.windowsChanged() ? dc[kDcIntStatus] & (kWinAUnderflowInt | kWinBUnderflowInt) : 0;
    if (underflow)
        dc[kDcIntStatus] = underflow;
    bool asWritten = true;
    if (stage == Run::kSettle || stage == Run::kHold) {
        const display_window_probe::Step &step = s.plan.step[s.run.step];
        asWritten = windowProbeShows(step.a, step.b);
    } else if (stage == Run::kRestoreLatch) {
        asWritten = windowProbeRestoredOk();
    }
    if (stage == Run::kWaiting)
        s.busyReason = columnScanInFlight();
    const uint32_t crc = dc[kDcCrcLatched];
    const bool requestOut = (dc[kDcStateControl] & (kGeneralActReq | kWinAActReq | kWinBActReq)) != 0u;
    const Run::Action action = s.run.frameEnd(s.busyReason == nullptr, requestOut, crc, underflow != 0u, asWritten);
    windowProbePerform(action);
    if (action == Run::kFinish && kickDeferred) {  // the swaps held since the request go
        kickDeferred = false;
        kickSwapQueue();
    }
}

void windowProbeFinish()
{
    WindowProbeState &s = windowProbe;
    s.extras.restore(dc);
    if (s.saved) {
        windowProbeWriteUnshadowed(s.savedCsc, s.savedHTaps, s.savedVTaps);
        s.saved = false;
    }
    if (s.reserved) {  // window B is off again: the planes' latched set is the whole demand
        planeRetireBandwidthTo();
        s.reserved = false;
    }
    if (s.run.why == display_window_probe::Run::kNeverQuiet && s.busyReason) {
        snprintf(s.why, sizeof(s.why), "never quiet: %s", s.busyReason);
        s.run.why = s.why;
    }
    gPanicRestore.columnScan = false;
    windowProbeSummary();
    if (s.run.restored && s.slot >= 0) {  // nothing scans the pictures any more
        planeDropSlot(static_cast<unsigned>(s.slot));
        OSSafeReleaseNULL(s.buffer);
    }
    s.slot = -1;
    s.claimed = false;
    if (s.holding) {
        s.holding = false;
        native_legacy_frame_end_release();
    }
}

// One string: the kernel's os_log drops a record whose scalar arguments outgrow its 256-byte buffer.
void windowProbeSummary()
{
    const WindowProbeState &s = windowProbe;
    const display_window_probe::Finding finding = display_window_probe::judge(s.plan, s.run);
    char line[512];
    int at = snprintf(line, sizeof(line), "%s %s", display_window_probe::probeName(s.plan.probe),
                      display_window_probe::verdictName(finding.verdict));
    for (unsigned i = 0; i < s.plan.steps && at > 0 && static_cast<size_t>(at) < sizeof(line); ++i) {
        const display_window_probe::StepResult &r = s.run.result[i];
        at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "; %s %#x%s%s", s.plan.step[i].name, r.crc,
                       r.settled ? "" : " unsettled", r.underflows || r.changes || r.mismatches ? " unclean" : "");
    }
    IOLog("SwitchDisplay: window probe %s; %s; windows %s\n", line, s.run.why ? s.run.why : finding.note,
          s.run.restored         ? "restored and read back"
          : s.run.restoreWritten ? "restore written, not confirmed"
                                 : "untouched");
}

// The display stops, sleeps or goes off mid-run (no frame end will come): the saved registers go into the assembly
// copy now and latch when the scan runs again.
void windowProbeAbort(const char *reason)
{
    if (!windowProbe.run.busy())
        return;
    if (windowProbe.run.abort(reason))
        windowProbeRestoreWindows();
    windowProbeFinish();
}
#else
static bool windowProbeBusy()
{
    return false;
}

static bool windowProbeHolds()
{
    return false;
}

void windowProbeFrameEnd()
{}

void windowProbeAbort(const char *)
{}
#endif
