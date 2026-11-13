// SPDX-License-Identifier: BSD-2-Clause-Patent
uint64_t oldestPendingFrame() const
{
    uint64_t first = 0;
    const auto include = [&first](uint64_t sequence) {
        if (sequence && (!first || sequence < first))
            first = sequence;
    };
    const int entries[] = {ring.pending, ring.superseded, ring.held};
    for (const int entry : entries)
        if (entry != kNoBuffer)
            include(entryPendingSeq[entry] ? entryPendingSeq[entry] : entrySeq[entry]);
    const auto plane = [this, &include](const PlaneSet &set) {
        if (!planesAbandoned)
            include(set.pendingSeq ? set.pendingSeq : set.seq);
    };
    if (planeRecovering)
        plane(planeRecoveryTarget);
    if (planeLatch.requested())
        plane(planeSetOf(planeLatch.armed));
    if (planeLatch.superseded >= 0)
        plane(planeSetOf(planeLatch.superseded));
    if (planeHeldSet)
        plane(planeHeld);
    if (planeFencePending)
        plane(planeFencing);
    if (landscapeState.wanted)
        plane(landscapeState.held);
    return first;
}

// A swap that waited for a frame end (a busy overlay source, a scanned slot) goes again at one where nothing else
// will kick: no window A flip or overlay set out, no console scroll, probe or overlay recovery in progress.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
void kickAtIdleFrameEnd()
{
    if (!kickDeferred || ring.pending != kNoBuffer || planeLatch.requested() || tapeSettle || probeRequest ||
        probe.active() || planeProbe.active() || planeRecovering || hwSuspended || scanoutStopped)
        return;
    kickDeferred = false;
    kickSwapQueue();
}

// At a latch (latchRetire): every taken swap at the head up to the one seen on screen (take sequence seq) retires, in queue
// order; the one still in flight, or a newer one asked for, stays. shownAt: when the screen began showing it (mach time).
// This retirement is what 24B119 QuartzCore's page check reads. IOMFBDisplay::can_update_ gives a page back once the
// swap after it completed: swap_wait(N, 0x80000001), polled every 100 us for up to 1 ms (swap_wait_timeout,
// 0x184322b14-0x184322b48), so kIOReturnBusy there only means swap N is not on screen yet. Its page count is a
// constant 3 that no IOMFB answer sizes (AppleDisplay's constructor, 0x1844e7004-0x1844e701c; 1 only in benchmark
// mode, 0x1844e6798-0x1844e67a8), and the "swap_wait (N, flags 2147483649) = 0xe00002d5" lines print only after more
// than 3 failed can_update calls in a row (display+0x75b8, 0x184321c40-0x184321c5c and 0x184321cec-0x184321cf4):
// they name swaps 4 or more frames late.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
void retireThrough(uint64_t seq, uint64_t shownAt)
{
    if (latchRetire) {
        scanoutRetirement.observe(seq, shownAt);
        const auto ready = scanoutRetirement.take(oldestPendingFrame());
        if (!ready.sequence)
            return;
        seq = ready.sequence;
        shownAt = ready.shownAt;
    }
    frameShown(seq, shownAt);
    if (!latchRetire || !seq)
        return;
    bool retired = false;
    for (const OriginalLegacySwapRequest *head = pendingSwapRequest(); head; head = pendingSwapRequest()) {
        const uint64_t at = seqOf(head);
        if (!at || at > seq || (gpuScanout && head == gpuRequest))
            break;
        if (!retireSwap(head, true))
            break;
        retired = true;
    }
    if (retired)
        kickSwapQueue();
}
