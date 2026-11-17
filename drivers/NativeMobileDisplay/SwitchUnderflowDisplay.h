// SPDX-License-Identifier: BSD-2-Clause-Patent
// The direct copies of each window's UFLOW_STATUS (L4T dc_reg.h DC_WINBUF_{A,B,C}D_UFLOW_STATUS): read whatever
// WINDOW_HEADER selects.
static constexpr unsigned kUnderflowWindows = switch_display::UnderflowAccounting::kWindows;
static constexpr uint32_t kUnderflowCount[kUnderflowWindows] = {0xbca, 0xdca, 0xfca};
static constexpr uint32_t kUnderflowLatch[kUnderflowWindows] = {kWinAUnderflowInt, kWinBUnderflowInt,
                                                                kWinCUnderflowInt};
static constexpr uint32_t kDcIntType = 0x03a, kDcIntPolarity = 0x03b;
switch_display::UnderflowAccounting underflows;
uint32_t underflowCounts[kUnderflowWindows] = {};
bool underflowLatched[kUnderflowWindows] = {}, underflowFirstRead = false;

// Every DC A frame end, before checkRotation and planeFrameEnd change what scans: every count is read and cleared, and
// kept only for a window that scanned for the driver through the frame that ended. The boot chain's counts and a
// window's counts from before it scanned for us are dropped this way.
void underflowRead()
{
    const PlaneSet &overlays = planeSetOf(planeLatch.active);
    const bool scanned[kUnderflowWindows] = {rotationLatched, planesAttached && overlays.window[0].on,
                                             planesAttached && overlays.window[1].on};
    uint32_t counts[kUnderflowWindows] = {};
    for (unsigned w = 0; w != kUnderflowWindows; ++w) {
        counts[w] = dc[kUnderflowCount[w]];
        if (counts[w])
            dc[kUnderflowCount[w]] = 0;
        underflowCounts[w] = scanned[w] && !scanoutStopped ? counts[w] : 0u;
    }
    // A count the boot chain left shows whether these counters run at all on this DC.
    if (stats && !underflowFirstRead) {
        underflowFirstRead = true;
        IOLog("SwitchDisplay: UFLOW_STATUS at the driver's first frame end A %#x B %#x C %#x; INT_TYPE %#x "
              "INT_POLARITY %#x\n",
              counts[0], counts[1], counts[2], static_cast<uint32_t>(dc[kDcIntType]),
              static_cast<uint32_t>(dc[kDcIntPolarity]));
    }
}

void clearUnderflowCounts()
{
    for (const uint32_t reg : kUnderflowCount)
        dc[reg] = 0;
}

// checkRotation's and planeFrameEnd's WIN_x_UF_INT, seen and cleared at this frame end.
void noteLatchedUnderflow(uint32_t latched)
{
    for (unsigned w = 0; w != kUnderflowWindows; ++w)
        underflowLatched[w] = underflowLatched[w] || (latched & kUnderflowLatch[w]) != 0u;
}

// Every DC A frame end, after planeFrameEnd: one line when a window starts underflowing and one when it has been clean
// for kQuietFrames frame ends.
void underflowFrameEnd()
{
    using Accounting = switch_display::UnderflowAccounting;
    for (unsigned w = 0; w != kUnderflowWindows; ++w) {
        const uint32_t count = underflowCounts[w];
        const bool latched = underflowLatched[w];
        underflowLatched[w] = false;
        const char name = static_cast<char>('A' + w);
        switch (underflows.frameEnd(w, count, latched)) {
        case Accounting::Edge::kOpened:
            IOLog("SwitchDisplay: window %c underflowed: UFLOW_STATUS %#x, WIN_%c_UF_INT %s, frame end %llu\n", name,
                  count, name, latched ? "latched" : "clear", frameEnds);
            if (w == 0 && landscapeOn())
                logColumnScanUnderflow();
            break;
        case Accounting::Edge::kClosed: {
            const Accounting::Totals &episode = underflows.episode(w);
            IOLog("SwitchDisplay: window %c underflow over: %u frame ends underflowed, %llu counted%s, then %u clean\n",
                  name, episode.frames, static_cast<unsigned long long>(episode.counted),
                  episode.overflowed ? " (count overflowed)" : "", Accounting::kQuietFrames);
            break;
        }
        case Accounting::Edge::kNone:
            break;
        }
    }
}

// Window A's active words while it column-scans the video (TRM 24.10.1-24.10.10, 24.11.4-24.11.7, 24.11.26-24.11.27):
// OPTIONS, SIZE, PRESCALED_SIZE, DDA_INCREMENT, H/V_INITIAL_DDA, ADDR_H/V_OFFSET, SURFACE_KIND, MEMFETCH_CONTROL,
// MEMFETCH_DEBUG_STATUS.
static constexpr uint32_t kColumnScanWords[] = {0xb80, 0xb85, 0xb86, 0xb89, 0xb87, 0xb88,
                                                0xbc6, 0xbc8, 0xbcb, 0xbeb, 0xbea};

void logColumnScanUnderflow()
{
    uint32_t v[sizeof(kColumnScanWords) / sizeof(kColumnScanWords[0])];
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); ++i)
        v[i] = dc[kColumnScanWords[i]];
    dc[kDcStateAccess] = access;
    IOLog("SwitchDisplay: window A column-scans the video: OPTIONS %#x SIZE %#x PRESCALED %#x DDA %#x INITIAL %#x/%#x "
          "OFFSET %#x/%#x KIND %#x MEMFETCH_CONTROL %#x MEMFETCH_DEBUG_STATUS %#x\n",
          v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10]);
}
