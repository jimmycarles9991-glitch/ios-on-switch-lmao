// SPDX-License-Identifier: BSD-2-Clause-Patent
// An accepted swap must present. One the kick still defers at a later frame end than its first deferral is held past a
// frame: one error line names what holds it and what that read, and one more says when it was taken or left the queue
// head (SwitchDisplayRetirement.h's HeldHead). Holds with bounds and lines of their own record nothing here: a
// requested presentation time (its horizon), a change of mode (its quiet frame end and latch bounds), a probe's or a
// display experiment's run. Included in the class body; under the gate.

static inline switch_display::HeldHead heldHead;  // static: the instance is at the 16 KiB a kalloc_type class may have

uint32_t headSwap(const OriginalLegacySwapRequest *request) const
{
    uint32_t id = 0;
    memcpy(&id, reinterpret_cast<const unsigned char *>(request) + swapId, sizeof(id));
    return id;
}

bool consoleHeld() const
{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    return consoleHolds();
#else
    return false;
#endif
}

void endHeldHead(const switch_display::HeldHead::Ended &ended)
{
    if (!ended.swap)
        return;
    IOLog("SwitchDisplay: swap %u %s after %llu ms at the queue head\n", ended.swap,
          ended.taken ? "taken" : "left untaken (the family let it go)",
          static_cast<unsigned long long>(ticksNs(ended.ticks) / 1000000));
}

// kickOnce leaves `request` at the queue head because of `why` (a plane slot's wait gives its code).
void deferHead(const OriginalLegacySwapRequest *request, const char *why, int code = 0)
{
    kickDeferred = true;
    const uint64_t now = mach_absolute_time();
    switch_display::HeldHead::Ended ended;
    const bool held = heldHead.defer(headSwap(request), frameEnds, now, ended);
    endHeldHead(ended);
    if (!held)
        return;
    const uint64_t accepted = submittedAt(request);
    IOLog("SwitchDisplay: swap %u held at the queue head past a frame: %s (%d); landscape wanted %d switching %d, "
          "console %d; accepted %llu ms ago; %u held since boot\n",
          headSwap(request), why, code, landscapeState.wanted ? 1 : 0, landscapeState.switching ? 1 : 0,
          consoleHeld() ? 1 : 0,
          static_cast<unsigned long long>(accepted && now > accepted ? ticksNs(now - accepted) / 1000000 : 0),
          heldHead.heldSinceBoot());
}

void takeHeldHead(const OriginalLegacySwapRequest *request, uint64_t kickAt)
{
    if (heldHead.recording())
        endHeldHead(heldHead.take(headSwap(request), kickAt));
}
