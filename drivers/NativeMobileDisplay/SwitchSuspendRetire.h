// SPDX-License-Identifier: BSD-2-Clause-Patent
// SC7 takes what is asked for as shown, with no frame end to check it: ring.settle makes window A's pending flip the
// front, and planesQuiesce's settle makes windows B and C's requested (or replaced) set the active one. Both are what the
// saved controller gets back at the resume. quiesceForSuspend reads their frames before those settles and retires through
// them once the controller is saved, with the latch's own retirement stamped at the settle, as settleStopped does for a
// stopped scanout. Without it their swaps would complete only at the first latch after the resume, and never if nothing
// is presented then. Included in the class body after SwitchRetireDisplay.h.

uint64_t suspendShownSeq() const
{
    const uint64_t window = ring.pending != kNoBuffer ? entrySeq[ring.pending] : 0;
    const int taken = planeLatch.armed >= 0 && planeLatch.armedRequested ? planeLatch.armed : planeLatch.superseded;
    const uint64_t planes = planesOn && taken >= 0 ? planeSetOf(taken).seq : 0;
    return window > planes ? window : planes;
}

// After hwSuspended: a kick the retirement makes waits for the resume.
void retireSuspendShown(uint64_t seq)
{
    if (seq)
        retireThrough(seq, mach_absolute_time());
}
