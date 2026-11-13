// SPDX-License-Identifier: BSD-2-Clause-Patent
bool planeRecovering = false, planesAbandoned = false;
uint64_t planeRecoveryAt = 0;
PlaneSet planeRecoveryTarget = {};

void holdPlaneRecovery(const PlaneSet &p)
{
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
        IOLog("SwitchDisplay: the overlay recovery leaves landscape: landscape off this boot\n");
        landscapeState.retired = true;
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

// Windows B and C did not take their off request: they may still scan every slot their sets name, so nothing of theirs
// is released for the rest of the boot, no overlay is asked for again, and the swaps they carried retire without them.
void abandonPlanes()
{
    IOLog("SwitchDisplay: overlay windows did not stop in %u frames: overlays off for this boot, their memory kept\n",
          kLatchFrames);
    __atomic_store_n(&planesOn, false, __ATOMIC_RELEASE);
    planesAbandoned = true;
    planeRecovering = false;
    planeRecoveryTarget = {};
    planeHeldSet = false;
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
        if (frameEnds - planeRecoveryAt >= kLatchFrames)
            abandonPlanes();
        return true;
    }
    // Only observed inactive windows permit replacing their live CSC/filter state or releasing old snapshots.
    planeLatch.reset();
    planeForgetUnshadowed();
    planeUnderflowRun = 0;
    planeRecovering = false;
    PlaneSet restore = planeRecoveryTarget;
    if (!planesOn || restore.landscape)
        restore.window[0] = restore.window[1] = {};
    restore.landscape = false;
    planeRecoveryTarget = {};
    planeFlipOnly(restore);
    planeReleaseIdle();
    return true;
}
