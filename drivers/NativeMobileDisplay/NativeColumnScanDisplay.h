// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's window A column-scan experiment (NativeColumnScan.h has the rules and their sources), debug
// kernels only: included in the class body after NativePlanesDisplay.h. debug.switch_display_column_scan starts one run;
// its value is the test surface's block height, log2 GOBs (BlueTool writes 4 when the Mac posts
// com.brunostjohn.switch.debug.column-scan, 1 for ...column-scan-nvdec). The trigger fills the surfaces on its own thread
// and maps them into two plane slots; everything after that runs at DC A's frame ends under the gate, swaps held, the
// frame end held routed. Its state is static: the instance is at the 16 KiB a kalloc_type class may have.

#if SWITCH_FRAME_TIMELINE
struct ColumnScanState {
    native_column_scan::Run run;
    IOBufferMemoryDescriptor *surface = nullptr, *reference = nullptr;
    bool surfaceFilled = false, referenceFilled = false, saved = false, holding = false;
    uint8_t yLog2 = 0, filledYLog2 = 0;
    int surfaceSlot = -1, referenceSlot = -1;
    const char *busyReason = nullptr;
    native_column_scan::Window scan = {};
    uint32_t savedWindow[sizeof(kWindowAState) / sizeof(kWindowAState[0])] = {};
    uint32_t savedStartU = 0, savedStartHiU = 0, savedCrcControl = 0, savedCsc[8] = {}, savedHTaps[16] = {},
             savedVTaps[16] = {};
};

static ColumnScanState columnScan;
static struct sysctl_oid columnScanOid;
static constexpr uint32_t kWinc = 0x500, kWinbufStartAddrU = 0x800 + native_planes::kStartAddrU,
                          kWinbufStartAddrHiU = 0x800 + native_planes::kStartAddrHiU;

static bool columnScanBusy()
{
    return columnScan.run.busy();
}

static int columnScanSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    int value = 0;
    int changed = 0;
    const int error = sysctl_io_number(req, 0, sizeof(value), &value, &changed);
    if (error || !changed)
        return error;
    if (value < 0 || value > native_column_scan::kMaxYLog2)
        return EINVAL;
    NativeLegacyDisplay *display = __atomic_load_n(&primaryDisplay, __ATOMIC_ACQUIRE);
    return display ? display->columnScanTrigger(static_cast<uint8_t>(value)) : ENODEV;
}

static void columnScanRegister()
{
    if (columnScanOid.oid_name)
        return;
    switch_sysctl_oid(&columnScanOid, &sysctl__debug_children, "switch_display_column_scan",
                      CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, columnScanSysctl, "I",
                      "window A column-scans a 4K NV12 test frame for 30 frames: its block height, log2 GOBs");
    sysctl_register_oid(&columnScanOid);
}

template <typename Body> void columnScanOnLoop(Body body)
{
    getWorkLoop()->runAction(
        [](OSObject *, void *b, void *, void *, void *) -> IOReturn {
            (*static_cast<Body *>(b))();
            return kIOReturnSuccess;
        },
        this, &body);
}

// From the sysctl's thread. The run is claimed (kPreparing) before the surfaces are touched: no run reads them then.
int columnScanTrigger(uint8_t yLog2)
{
    if (!getWorkLoop())
        return ENODEV;
    int result = EBUSY;
    columnScanOnLoop([&] {
        if (columnScan.run.stage == native_column_scan::Run::kIdle && !windowProbeBusy()) {
            columnScan.run.stage = native_column_scan::Run::kPreparing;
            result = 0;
        }
    });
    if (result)
        return result;
    result = columnScanPrepare(yLog2);
    columnScanOnLoop([&] {
        if (!result)
            result = columnScanPost(yLog2);
        if (result)
            columnScan.run.stage = native_column_scan::Run::kIdle;
    });
    return result;
}

// Both surfaces allocated once and kept for the boot; the NV12 one refilled when the block height changes. DC A does
// not snoop the CPU's caches.
static int columnScanPrepare(uint8_t yLog2)
{
    ColumnScanState &s = columnScan;
    if (!s.surface) {
        s.surface = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
                                                                native_column_scan::kSurfaceBytes, PAGE_SIZE);
    }
    if (!s.reference) {
        s.reference = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
                                                                  native_column_scan::kReferenceBytes, PAGE_SIZE);
    }
    if (!s.surface || !s.reference) {
        IOLog("SwitchDisplay: column scan not run: no memory for its %llu-byte surface and %llu-byte reference\n",
              static_cast<unsigned long long>(native_column_scan::kSurfaceBytes),
              static_cast<unsigned long long>(native_column_scan::kReferenceBytes));
        return ENOMEM;
    }
    if (!s.referenceFilled) {
        uint8_t *frame = static_cast<uint8_t *>(s.reference->getBytesNoCopy());
        native_column_scan::fillReference(frame);
        switch_dcache_clean_invalidate(frame, native_column_scan::kReferenceBytes);
        s.referenceFilled = true;
    }
    if (!s.surfaceFilled || s.filledYLog2 != yLog2) {
        uint8_t *surface = static_cast<uint8_t *>(s.surface->getBytesNoCopy());
        native_column_scan::fillSurface(surface, yLog2);
        switch_dcache_clean_invalidate(surface, native_column_scan::kSurfaceBytes);
        s.surfaceFilled = true;
        s.filledYLog2 = yLog2;
    }
    return 0;
}

// Under the gate: what the run needs that waiting cannot bring, then both surfaces into plane slots (window A reads
// through the display's SMMU domain once direct scanout is on); swaps are held from here.
int columnScanPost(uint8_t yLog2)
{
    ColumnScanState &s = columnScan;
    const char *why = nullptr;
    if (!SWITCH_PORTRAIT_NATIVE || !rotated || !rotationLatched) {
        why = "window A does not scan the UI's block-linear frames yet";
    } else if (!directAttached) {
        why = "window A does not read through the SMMU (direct scanout is off)";
    } else if (!planesOn) {
        why = "planes are off: no IOVA slot to map the surfaces into";
    } else if (hwSuspended || scanoutStopped) {
        why = "the display is off";
    }
    if (!why) {
        s.surfaceSlot = planeSlotFor(s.surface, 0, native_column_scan::kSurfaceBytes, false);
        s.referenceSlot = planeSlotFor(s.reference, 0, native_column_scan::kReferenceBytes, false);
        if (s.surfaceSlot < 0 || s.referenceSlot < 0) {
            why = "no free plane slot";
        } else if (!native_column_scan::scanWindow(planeSlotIova(s.surfaceSlot), yLog2, s.scan)) {
            why = "window A cannot column-scan the surface";
        }
    }
    if (why) {
        IOLog("SwitchDisplay: column scan not run: %s\n", why);
        return EAGAIN;
    }
    s.yLog2 = yLog2;
    s.busyReason = nullptr;
    s.run.begin();
    s.holding = true;
    native_legacy_frame_end_hold();
    IOLog("SwitchDisplay: column scan requested: 3840x2160 NV12 in %u-GOB blocks at IOVA %#x, reference at %#x\n",
          1u << yLog2, s.scan.start, planeSlotIova(s.referenceSlot));
    return 0;
}

// Why window A or B may still change under the run (nullptr: nothing in flight).
const char *columnScanInFlight()
{
    if (transferred || copyQueued)
        return "a swap is in flight";
    if (ring.pending != kNoBuffer || ring.drawing != kNoBuffer || ring.held != kNoBuffer ||
        ring.superseded != kNoBuffer)
        return "a window A flip is out";
    if (probeRequest || probe.active() || planeProbe.active())
        return "an SMMU probe runs";
    if (planeLatch.requested() || planeOnScreen() || planeHeldSet)
        return "window B shows a plane";
    if (tapeAttached || tapeSettle)
        return "the console tape is on";
    if (hwSuspended || scanoutStopped)
        return "the display is off";
    if (dc[kDcStateControl] & (kGeneralActReq | kWinAActReq | kWinBActReq))
        return "a request is out";
    if (otherWindowEnabled())
        return "window B or C is enabled";
    return nullptr;
}

native_column_scan::Window columnScanActive()
{
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    dc[kDcWindowHeader] = kWindowASelect;
    native_column_scan::Window w;
    w.options = dc[kWinOptions];
    w.depth = dc[kWinColorDepth];
    w.position = dc[kWinPosition];
    w.size = dc[kWinSize];
    w.prescaled = dc[kWinPrescaled];
    w.hInitial = dc[kWinHInitialDda];
    w.vInitial = dc[kWinVInitialDda];
    w.dda = dc[kWinDdaIncrement];
    w.lineStride = dc[kWinLineStride];
    w.kind = dc[kWinbufSurfaceKind];
    w.start = dc[kWinbufStartAddr];
    w.startU = dc[kWinbufStartAddrU];
    w.hOffset = dc[kWinbufHOffset];
    w.vOffset = dc[kWinbufVOffset];
    dc[kDcStateAccess] = access;
    return w;
}

// The active copy of everything the run changes; the CSC and filter taps have only the one copy.
void columnScanSave()
{
    ColumnScanState &s = columnScan;
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    dc[kDcWindowHeader] = kWindowASelect;
    for (size_t i = 0; i < sizeof(kWindowAState) / sizeof(kWindowAState[0]); ++i)
        s.savedWindow[i] = dc[kWindowAState[i]];
    s.savedStartU = dc[kWinbufStartAddrU];
    s.savedStartHiU = dc[kWinbufStartAddrHiU];
    dc[kDcStateAccess] = access;
    s.savedCrcControl = dc[kDcCrcControl];
    for (unsigned i = 0; i < 8; ++i)
        s.savedCsc[i] = dc[kWinc + native_planes::kCscYof + i];
    for (unsigned i = 0; i < 16; ++i) {
        s.savedHTaps[i] = dc[kWinc + native_planes::kHFilter0 + i];
        s.savedVTaps[i] = dc[kWinc + native_planes::kVFilter0 + i];
    }
    s.saved = true;
}

bool columnScanRestoredOk()
{
    const ColumnScanState &s = columnScan;
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    dc[kDcWindowHeader] = kWindowASelect;
    bool same = dc[kWinbufStartAddrU] == s.savedStartU && dc[kWinbufStartAddrHiU] == s.savedStartHiU;
    for (size_t i = 0; i < sizeof(kWindowAState) / sizeof(kWindowAState[0]); ++i)
        same = same && dc[kWindowAState[i]] == s.savedWindow[i];
    dc[kDcStateAccess] = access;
    return same;
}

// Window A's assembly registers (WIN_OPTIONS last) and the CRC unit, latched together at the next frame boundary.
void columnScanShow(const native_column_scan::Window &w)
{
    withdrawCompletionFlip();
    dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcWindowHeader] = kWindowASelect;
    dc[kWinColorDepth] = w.depth;
    dc[kWinPosition] = w.position;
    dc[kWinSize] = w.size;
    dc[kWinPrescaled] = w.prescaled;
    dc[kWinHInitialDda] = w.hInitial;
    dc[kWinVInitialDda] = w.vInitial;
    dc[kWinDdaIncrement] = w.dda;
    dc[kWinLineStride] = w.lineStride;
    dc[kWinbufSurfaceKind] = w.kind;
    dc[kWinbufStartAddr] = w.start;
    dc[kWinbufStartAddrHi] = 0;
    dc[kWinbufStartAddrU] = w.startU;
    dc[kWinbufStartAddrHiU] = 0;
    dc[kWinbufHOffset] = w.hOffset;
    dc[kWinbufVOffset] = w.vOffset;
    dc[kWinOptions] = w.options;
    dc[kDcCrcControl] = kCrcOn;
    columnScanRequest();
}

void columnScanRequest()
{
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
}

void columnScanRestoreWindow()
{
    withdrawCompletionFlip();
    const ColumnScanState &s = columnScan;
    dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcWindowHeader] = kWindowASelect;
    dc[kWinbufStartAddrU] = s.savedStartU;
    dc[kWinbufStartAddrHiU] = s.savedStartHiU;
    for (size_t i = 0; i < sizeof(kWindowAState) / sizeof(kWindowAState[0]); ++i)
        dc[kWindowAState[i]] = s.savedWindow[i];
    dc[kDcCrcControl] = s.savedCrcControl;
    columnScanRequest();
}

// The CSC and filter taps are not shadowed: written only while window A scans RGB unfiltered.
void columnScanWriteUnshadowed(const uint32_t *csc, const uint32_t *hTaps, const uint32_t *vTaps)
{
    for (unsigned i = 0; i < 8; ++i)
        dc[kWinc + native_planes::kCscYof + i] = csc[i];
    for (unsigned i = 0; i < 16; ++i) {
        dc[kWinc + native_planes::kHFilter0 + i] = hTaps[i];
        dc[kWinc + native_planes::kVFilter0 + i] = vTaps[i];
    }
}

void columnScanPerform(native_column_scan::Run::Action action)
{
    const ColumnScanState &s = columnScan;
    switch (action) {
    case native_column_scan::Run::kNothing:
        return;
    case native_column_scan::Run::kStart: {
        columnScanSave();
        const uint32_t depth = windowActive(kWinColorDepth);
        gPanicRestore.columnScanDepth = depth;
        __asm__ volatile("dmb ish" ::: "memory");
        gPanicRestore.columnScan = true;
        columnScanShow(native_column_scan::referenceWindow(
            planeSlotIova(s.referenceSlot),
            depth == native_planes::kDepthR8G8B8A8 ? depth : native_planes::kDepthB8G8R8A8));
        return;
    }
    case native_column_scan::Run::kShowScan: {
        uint32_t vTaps[16];
        for (unsigned i = 0; i < 16; ++i)
            vTaps[i] = native_planes::vFilterTap(i);
        columnScanWriteUnshadowed(native_planes::kCsc[native_planes::kBt709][1], native_planes::kHFilterTaps, vTaps);
        columnScanShow(s.scan);
        return;
    }
    case native_column_scan::Run::kRestore:
        columnScanRestoreWindow();
        return;
    case native_column_scan::Run::kFinish:
        columnScanFinish();
        return;
    }
}

// At every DC A frame end, before checkRotation: window A's underflow during the run is the run's.
void columnScanFrameEnd()
{
    ColumnScanState &s = columnScan;
    if (!s.run.busy())
        return;
    const native_column_scan::Run::Stage stage = s.run.stage;
    const uint32_t underflow = s.run.windowChanged() ? dc[kDcIntStatus] & kWinAUnderflowInt : 0;
    if (underflow)
        dc[kDcIntStatus] = underflow;
    const native_column_scan::Window active = columnScanActive();
    bool asWritten = true;
    if (stage == native_column_scan::Run::kScan) {
        asWritten = native_column_scan::sameWindow(active, s.scan);
    } else if (stage == native_column_scan::Run::kRestoreLatch) {
        asWritten = columnScanRestoredOk();
    }
    if (stage == native_column_scan::Run::kWaiting)
        s.busyReason = columnScanInFlight();
    const uint32_t crc = dc[kDcCrcLatched];
    const bool requestOut = (dc[kDcStateControl] & (kGeneralActReq | kWinAActReq)) != 0u;
    const native_column_scan::Run::Action action =
        s.run.frameEnd(s.busyReason == nullptr, requestOut, crc, underflow != 0u, asWritten);
    if (stage == native_column_scan::Run::kScan)
        columnScanLogFrame(crc, underflow, active, asWritten);
    columnScanPerform(action);
    if (action == native_column_scan::Run::kFinish && kickDeferred) {  // the swaps held since the request go
        kickDeferred = false;
        kickSwapQueue();
    }
}

// One string: the kernel's os_log drops a record whose scalar arguments outgrow its 256-byte buffer.
static void columnScanLogFrame(uint32_t crc, uint32_t underflow, const native_column_scan::Window &w, bool asWritten)
{
    char line[320];
    snprintf(line, sizeof(line),
             "frame %u CRC %#x UF %u; active OPTIONS %#x DEPTH %u POS %#x SIZE %#x PRESCALED %#x DDA %#x INIT %#x/%#x "
             "STRIDE %#x KIND %#x START %#x U %#x H %u V %u (%s)",
             columnScan.run.scanFrames, crc, underflow ? 1u : 0u, w.options, w.depth, w.position, w.size, w.prescaled,
             w.dda, w.hInitial, w.vInitial, w.lineStride, w.kind, w.start, w.startU, w.hOffset, w.vOffset,
             asWritten ? "as written" : "NOT as written");
    os_log(OS_LOG_DEFAULT, "SwitchDisplay column scan: %s", line);
}

void columnScanFinish()
{
    ColumnScanState &s = columnScan;
    if (s.saved) {
        columnScanWriteUnshadowed(s.savedCsc, s.savedHTaps, s.savedVTaps);
        s.saved = false;
    }
    gPanicRestore.columnScan = false;
    columnScanSummary();
    if (s.holding) {
        s.holding = false;
        native_legacy_frame_end_release();
    }
}

static void columnScanSummary()
{
    const native_column_scan::Run &r = columnScan.run;
    char verdict[200];
    if (r.exact()) {
        snprintf(verdict, sizeof(verdict), "PASS, pixel-exact: its CRC is the reference frame's");
    } else if (r.clean()) {
        snprintf(verdict, sizeof(verdict),
                 "PASS, clean but not pixel-exact: stable, no underflow, registers as written, CRC not the "
                 "reference frame's");
    } else {
        snprintf(verdict, sizeof(verdict), "FAILED at %s: %s%s%s",
                 native_column_scan::Run::name(r.why ? r.failedAt : native_column_scan::Run::kScan),
                 r.why ? r.why : "unstable, underflowing or not as written", columnScan.busyReason ? ", " : "",
                 columnScan.busyReason ? columnScan.busyReason : "");
    }
    const char *windowA = "untouched";
    if (r.restored) {
        windowA = "restored and read back";
    } else if (r.restoreWritten) {
        windowA = "restore written, not confirmed";
    }
    IOLog("SwitchDisplay: column scan %s; %u-GOB blocks; reference CRC %#x (%u underflow frames), column scan CRC "
          "%#x (%u changes over frames %u-%u), %u underflow frames, registers not as written in %u of %u frames; "
          "window A %s\n",
          verdict, 1u << columnScan.yLog2, r.reference, r.referenceUnderflows, r.scanCrc, r.crcChanges,
          native_column_scan::Run::kTransitionFrames + 1, native_column_scan::Run::kScanFrames, r.underflows,
          r.mismatches, r.scanFrames, windowA);
}

// The display stops, sleeps or goes off mid-run (no frame end will come): window A's saved registers go into the
// assembly copy now and latch when the scan runs again.
void columnScanAbort(const char *reason)
{
    if (!columnScan.run.busy())
        return;
    if (columnScan.run.abort(reason))
        columnScanRestoreWindow();
    columnScanFinish();
}
#else
static bool columnScanBusy()
{
    return false;
}

static void columnScanRegister()
{}

void columnScanFrameEnd()
{}

void columnScanAbort(const char *)
{}
#endif
