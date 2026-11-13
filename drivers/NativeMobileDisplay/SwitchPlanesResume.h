// SPDX-License-Identifier: BSD-2-Clause-Patent
// Overlays whose bandwidth cannot be restored resume off, as an overlay recovery's target: the UI is shown alone and the
// swaps they carried retire at the first latch.
void planesResume()
{
    if (!planesOn)
        return;
    bool recovering = planeRecovering;
    PlaneSet now = recovering ? planeRecoveryTarget : planeSetOf(planeLatch.active);
    uint32_t update = kGeneralUpdate | kWinAUpdate | kWinBUpdate | kWinCUpdate;
    uint32_t request = kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq;
    if (!planeReserveBandwidth(now)) {
        IOLog("SwitchDisplay: overlay bandwidth not restored after SC7: the overlays resume off\n");
        now.window[0].on = now.window[1].on = false;
        recovering = true;
    }
    if (recovering) {
        planeLatch.reset();
        if (planeLatch.write(now) < 0)
            panic("SwitchDisplay: SC7 recovery target could not be restored");
        planeTarget = now;
        planeRecovering = false;
        planeRecoveryTarget = {};
        planeRequestedAt = frameEnds;
    }
    planeStartRegisters();
    for (unsigned window = 0; window != 2; ++window)
        planeWriteWindowRegisters(planeWindowOf(now, window), now.window[window], false);
    const bool over = now.on();
    planeBlending = !over;  // force the write: the cold init put hekate's values back
    planeBlendA(over, update, request);
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
    if (planeHeldSet) {
        planeHeldSet = false;
        planeFlipOnly(planeHeld);
    }
}
