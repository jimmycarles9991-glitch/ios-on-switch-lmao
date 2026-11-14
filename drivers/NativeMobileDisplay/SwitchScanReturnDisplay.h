// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: whether window A ever scans a page Core Animation has back (SwitchScanReturn.h). A page is checked when
// it goes back (window A must neither scan it, nor have it asked for, replaced, held or armed for a completion flip),
// and at every frame end the page window A scans must not be one Core Animation has back, nor may window B have a
// surface in reach (shown, asked for or replaced) whose use count it does not hold (SwitchPlaneSurfaceUseDisplay.h).
// kern.switch_scan_{checks,handed,overlaps} count the checks, the pages handed back and the checks that failed, since
// boot (hw_test_app's display.scan_returned). The first overlap logs one line. Included in the class body after
// SwitchDirectSlots.h and SwitchCompletionFlipDisplay.h; under the gate.

#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kScanTotals = 3;
static inline uint32_t scanTotals[kScanTotals] = {};  // checks, handed back, overlaps
static inline struct sysctl_oid scanTotalOids[kScanTotals] = {};
static inline switch_display::ScanReturn scanReturn;
static inline const OriginalLegacySwapRequest *scanKicked = nullptr;
static inline const void *scanKickedPage = nullptr;
static inline const char scanBlank = 0;

// The kick's frame for window A: its surface's memory, the blank fill, or none (the swap leaves window A alone).
void scanReturnKick(const OriginalLegacySwapRequest *request, const void *page)
{
    scanKicked = request;
    scanKickedPage = page;
}

void scanReturnBlank(const OriginalLegacySwapRequest *request)
{
    scanReturnKick(request, &scanBlank);
}

void scanReturnTaken(const OriginalLegacySwapRequest *request)
{
    const void *page = scanKicked == request ? scanKickedPage : switch_display::ScanReturn::kUnknown;
    scanKicked = nullptr;
    scanReturn.taken(seqOf(request), page);
}

int scanEntryOf(const void *page) const
{
    for (unsigned k = 0; k < kSlots; ++k)
        if (page && slots[k].memory == page)
            return kSlot0 + static_cast<int>(k);
    return kNoBuffer;
}

const char *scanReach(int entry)
{
    if (entry == kNoBuffer)
        return nullptr;
    if (!hwSuspended && !scanoutStopped && surfaceIndex(uiActive(kWinbufStartAddr)) == entry)
        return "window A scans the page";
    if (entry == ring.pending)
        return "window A has the page asked for";
    if (entry == ring.superseded)
        return "a replaced flip to the page may have latched";
    if (entry == ring.held)
        return "the page waits for a flip";
    if (entry == ring.front)
        return "the ring has the page scanned";
    if (!completionFlipIdle() && completionFlip.entry == entry)
        return "the page's completion flip is out";
    return nullptr;
}

static uint32_t scanCount(unsigned which)
{
    return __atomic_fetch_add(&scanTotals[which], 1u, __ATOMIC_RELAXED);
}

void scanOverlap(const char *when, int entry, const char *why)
{
    if (scanCount(2))
        return;
    IOLog(
        "SwitchDisplay: %s, a buffer its owner has back is in reach: %s (%d; window A front %d pending %d replaced %d "
        "held %d)\n",
        when, why, entry, ring.front, ring.pending, ring.superseded, ring.held);
}

// retireSwap's first step: the page this retirement gives back must be out of window A's reach.
void scanReturnRetired(const OriginalLegacySwapRequest *request)
{
    const void *back = scanReturn.retired(seqOf(request));
    if (!back)
        return;
    scanCount(1);
    scanCount(0);
    const int entry = scanEntryOf(back);
    if (const char *why = scanReach(entry))
        scanOverlap("a swap retired", entry, why);
}

// A frame end with window A rotated, after its latch was read.
void scanReturnFrameEnd()
{
    if (hwSuspended || scanoutStopped || !rotationLatched)
        return;
    scanCount(0);
    const int entry = surfaceIndex(uiActive(kWinbufStartAddr));
    if (entry >= kSlot0 && scanReturn.back(slots[entry - kSlot0].memory))
        scanOverlap("a frame end", entry, "window A scans a page Core Animation has back");
    if (!planesOn || !planeUses)
        return;
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        if (!planeLatch.uses(static_cast<int>(k)))
            continue;
        scanCount(0);
        if (planeUses[k].noted && !planeUses[k].held)
            scanOverlap("a frame end", static_cast<int>(k), "window B may scan a surface it holds no use count on");
    }
}

static int scanTotalSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    const int value =
        static_cast<int>(__atomic_load_n(&scanTotals[static_cast<unsigned>(which) % kScanTotals], __ATOMIC_RELAXED));
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void scanTotalsRegister()
{
    static const char *const names[kScanTotals] = {"switch_scan_checks", "switch_scan_handed", "switch_scan_overlaps"};
    for (unsigned i = 0; i < kScanTotals; ++i) {
        if (scanTotalOids[i].oid_name)
            continue;
        switch_sysctl_oid(&scanTotalOids[i], &sysctl__kern_children, names[i],
                          CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, scanTotalSysctl, "I",
                          "window A since boot: checks that it cannot scan a page Core Animation has back, pages "
                          "handed back, checks that failed");
        scanTotalOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&scanTotalOids[i]);
    }
}
#else
void scanReturnKick(const OriginalLegacySwapRequest *, const void *)
{}

void scanReturnBlank(const OriginalLegacySwapRequest *)
{}

void scanReturnTaken(const OriginalLegacySwapRequest *)
{}

void scanReturnRetired(const OriginalLegacySwapRequest *)
{}

void scanReturnFrameEnd()
{}

static void scanTotalsRegister()
{}
#endif
