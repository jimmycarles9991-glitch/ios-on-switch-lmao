// SPDX-License-Identifier: BSD-2-Clause-Patent
bool planeRecovering = false, planeRecoveryStuck = false;
uint64_t planeRecoveryAt = 0;
PlaneSet planeRecoveryTarget = {};

// A recovery still inside its off request's latch window: frame ends stay unmasked for it and the flip deadline watches it.
// A stuck one is checked at the frame ends other work brings, and holds no swap's retirement.
bool planeRecoveryWaits() const
{
    return planeRecovering && !planeRecoveryStuck;
}

void holdPlaneRecovery(const PlaneSet &p)
{
    if (planeRecoveryStuck) {
        if (stats)
            ++stPlaneDropped;
        retireUnshown(p.seq);
        return;
    }
    uint64_t first = planeRecoveryTarget.pendingSeq ? planeRecoveryTarget.pendingSeq : planeRecoveryTarget.seq;
    const uint64_t incoming = p.pendingSeq ? p.pendingSeq : p.seq;
    if (!first || (incoming && incoming < first))
        first = incoming;
    if (p.seq >= planeRecoveryTarget.seq)
        planeRecoveryTarget = p;
    planeRecoveryTarget.pendingSeq = first;
    planeTarget = planeRecoveryTarget;
}

void writePlaneRecoveryOff()
{
    dc[kWinBBase + native_planes::kOptions] = 0;
    dc[kWinCBase + native_planes::kOptions] = 0;
    scanoutMemoryBarrier();
    dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate | kWinCUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinBActReq | kWinCActReq;
    planeRecoveryAt = frameEnds;
    armFlipDeadline();
}

void beginPlaneRecovery(const char *why)
{
    mediaFlipFromGate();
    if (planeRecovering)
        return;
    IOLog("SwitchDisplay: recovering overlay windows: %s\n", why);
    planeRecovering = true;
    planeRecoveryTarget = {};
    const int states[] = {planeLatch.active, planeLatch.superseded, planeLatch.armed};
    bool landscape = false;
    for (int state : states)
        if (state >= 0) {
            landscape = landscape || planeSetOf(state).landscape;
            holdPlaneRecovery(planeSetOf(state));
        }
    if (planeHeldSet) {
        holdPlaneRecovery(planeHeld);
        planeHeldSet = false;
    }
    if (landscape) {
        // The windows go back to portrait first; what went wrong was window A's column scan (or its reservation).
        ++landscapeState.failures;
        IOLog("SwitchDisplay: the overlay recovery leaves landscape\n");
        landscapeLeaveNow(why);
    } else {
        landscapeAbandonWant();
    }
    if (planeRecoveryTarget.landscape) {
        planeRecoveryTarget.landscape = false;
        planeRecoveryTarget.window[0] = planeRecoveryTarget.window[1] = {};
        planeTarget = planeRecoveryTarget;
    }
    writePlaneRecoveryOff();
}

// Windows B and C did not read off within kLatchFrames of their off request, the readback's bound: one error line. They
// may still scan every slot their sets name, so those slots stay mapped until a frame end reads both windows off (no
// request is written again); meanwhile no overlay is asked for and the swaps the held sets carried retire without them.
void planeRecoveryNotStopped()
{
    IOLog(
        "SwitchDisplay: overlay windows did not read off in %u frames: B OPTIONS %#x C OPTIONS %#x STATE_CONTROL %#x; "
        "their slots stay mapped until they do\n",
        kLatchFrames, planeRecoveryOptions(kWinBBase), planeRecoveryOptions(kWinCBase),
        static_cast<uint32_t>(dc[kDcStateControl]));
    planeRecoveryStuck = true;
    __atomic_store_n(&planesOn, false, __ATOMIC_RELEASE);
    const uint64_t seq = planeRecoveryTarget.seq;
    planeRecoveryTarget = {};
    planeTarget = {};
    planeHeldSet = false;
    if (seq)
        retireUnshown(seq);
}

uint32_t planeRecoveryOptions(uint32_t window)
{
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    const uint32_t options = dc[window + native_planes::kOptions];
    dc[kDcStateAccess] = access;
    return options;
}

bool planeRecoveryFrameEnd()
{
    if (!planeRecovering)
        return false;
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    const bool disabled = !(dc[kWinBBase + native_planes::kOptions] & native_planes::kWinEnable) &&
                          !(dc[kWinCBase + native_planes::kOptions] & native_planes::kWinEnable);
    dc[kDcStateAccess] = access;
    if (!disabled || (dc[kDcStateControl] & (kWinBActReq | kWinCActReq))) {
        if (!planeRecoveryStuck && frameEnds - planeRecoveryAt >= kLatchFrames)
            planeRecoveryNotStopped();
        return true;
    }
    // Only observed inactive windows permit replacing their live CSC/filter state or releasing old snapshots.
    planeLatch.reset();
    planeForgetUnshadowed();
    planeUnderflowRun = 0;
    planeRecovering = false;
    if (planeRecoveryStuck) {
        planeRecoveryStuck = false;
        __atomic_store_n(&planesOn, true, __ATOMIC_RELEASE);
    }
    PlaneSet restore = planeRecoveryTarget;
    if (!planesOn || restore.landscape)
        restore.window[0] = restore.window[1] = {};
    restore.landscape = false;
    planeRecoveryTarget = {};
    planeFlipOnly(restore);
    planeReleaseIdle();
    return true;
}
