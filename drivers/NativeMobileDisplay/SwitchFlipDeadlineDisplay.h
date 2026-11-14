// SPDX-License-Identifier: BSD-2-Clause-Patent
// The deadlines a lost DC A interrupt or a lost GPU completion panics at: one timer, armed while anything is out (a
// flip, a completion flip, an offer whose completion is looked for by its age) and looked at every kFlipDeadlineMS, not
// armed and cancelled per flip or per offer. checkFlipDeadline panics when a flip is still out and no frame end came
// since its last look; an offer's completion is looked for at frame ends and there, by the offer's age. Included in the
// class body (and in the host harness).

// Static, as the completion flip's state: the instance is at the 16 KiB a kalloc_type class may have, and one display
// runs it.
static inline bool flipDeadlineArmed = false;
static inline uint64_t flipDeadlineFrames = 0;  // frameEnds when the deadline was last armed
static inline bool offerAged = false;           // a direct or GPU-copy offer is out, due by gpuOfferDeadline
static inline uint64_t gpuOfferDeadline = 0;

// Under the gate, a request just written: checkFlipDeadline looks at it within kFlipDeadlineMS, or settles it at once
// while the scanout is stopped.
void armFlipDeadline()
{
    if (!flipTimer)
        return;
    if (scanoutStopped) {
        settleArmed = flipDeadlineArmed = true;
        flipTimer->setTimeoutMS(0);
        return;
    }
    armDeadlineTimer();
}

void armDeadlineTimer()
{
    if (flipDeadlineArmed || !flipTimer)
        return;
    flipDeadlineArmed = true;
    flipDeadlineFrames = frameEnds;
    flipTimer->setTimeoutMS(kFlipDeadlineMS);
}

// The last frame end of a burst, nothing out: nothing for the deadline to look at.
void idleFlipDeadline()
{
    if (!flipDeadlineArmed || !flipTimer)
        return;
    flipDeadlineArmed = settleArmed = false;
    flipTimer->cancelTimeout();
}

// An offer out: its completion is due kGpuCopyDeadlineMS from now.
void armGpuDeadline()
{
    gpuDeadlines = 0;
    offerAged = true;
    gpuOfferDeadline = mach_absolute_time() + gpuDeadlineTicks();
    armDeadlineTimer();
}

void cancelGpuDeadline()
{
    offerAged = false;
}

// The GPU driver still holds the offer, or its completion is signalled: it is looked for again a deadline later.
void extendGpuDeadline()
{
    if (offerAged)
        gpuOfferDeadline = mach_absolute_time() + gpuDeadlineTicks();
    else
        gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
}

void checkOfferAge()
{
    if (offerAged && gpuScanout && mach_absolute_time() >= gpuOfferDeadline)
        gpuDeadlineCheck();
}

// Under the gate, kFlipDeadlineMS after the deadline was armed. Frame ends since then mean DC A's interrupt is alive, and
// checkFlip judges every latch at them (four frame ends settle a flip or panic); none, with something out and the scanout
// running, means the interrupt is lost. A frame end taken by the filter but not yet handled (the gate was busy, e.g. with
// a copy) counts as one.
void checkFlipDeadline()
{
    flipDeadlineArmed = false;
    if (settleArmed) {
        settleArmed = false;
        if (!scanoutStopped) {
            armDeadlineTimer();  // restarted before the settle: the deadline runs from now
        } else if (!hwSuspended && !gPanicRestore.panicking) {
            settleStopped();
        }
        return;
    }
    checkOfferAge();
    const bool offerOut = offerAged && gpuScanout;
    if ((ring.pending == kNoBuffer && !probeRequest && !(planesOn && planeLatch.requested()) && !planeRecoveryWaits() &&
         !completionFlipOut()) ||
        hwSuspended || scanoutStopped || gPanicRestore.panicking) {
        if (offerOut)
            armDeadlineTimer();
        return;
    }
    // Frame ends came since the last look: DC A's interrupt is alive, and checkFlip judges each latch at them.
    if (frameEnds != flipDeadlineFrames || frameEndPending(kFrameEndInt)) {
        armDeadlineTimer();
        return;
    }
    if (planeRecoveryWaits())
        panic("SwitchDisplay: overlay recovery has no completed stop after %u ms", kFlipDeadlineMS);
    char gic[192];
    switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
    panic("SwitchDisplay: window A flip to %#x not settled in %u ms (%llu frame ends since): STATE_CONTROL %#x "
          "INT_STATUS %#x MASK %#x ENABLE %#x, %s",
          ring.pending != kNoBuffer ? surfaceAddress(ring.pending)
          : !completionFlipIdle()   ? completionFlip.start
          : probe.onIova            ? (uint32_t)kProbeSlot
                                    : surfaceAddress(probeSurface),
          kFlipDeadlineMS, frameEnds - flipRequested, static_cast<uint32_t>(dc[kDcStateControl]),
          static_cast<uint32_t>(dc[kDcIntStatus]), static_cast<uint32_t>(dc[kDcIntMask]),
          static_cast<uint32_t>(dc[kDcIntEnable]), gic);
}
