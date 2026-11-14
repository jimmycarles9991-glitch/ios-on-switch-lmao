// SPDX-License-Identifier: BSD-2-Clause-Patent
// The scanout domain's attach (SwitchScanoutSetup.h) when firmware left window B or C on: the dc swgroup translates all
// three windows at once, so a stray window would fetch through the SMMU. Its OPTIONS are cleared and requested at the
// start, and the attach finishes at the first frame end that reads both windows off with no request out. The readback's
// bound is kLatchFrames, for which frame ends stay unmasked; past it, one error line, and the attach waits for a frame end
// other work brings that reads them off. Included in the class body before SwitchScanoutSetup.h; runs under the work-loop
// gate.

// Static: the instance is at the 16 KiB a kalloc_type class may have.
static inline bool scanoutAttachWaits = false;
static inline uint64_t scanoutAttachAskedAt = 0;

bool scanoutAttachHoldsFrameEnds() const
{
    return scanoutAttachWaits && frameEnds - scanoutAttachAskedAt <= kLatchFrames;
}

void stopFirmwareWindows()
{
    dc[kWinBBase + native_planes::kOptions] = 0;
    dc[kWinCBase + native_planes::kOptions] = 0;
    dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate | kWinCUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinBActReq | kWinCActReq;
    scanoutAttachAskedAt = frameEnds;
}

// Every DC A frame end.
void scanoutAttachFrameEnd()
{
    if (!scanoutAttachWaits || hwSuspended)
        return;
    if (otherWindowEnabled() || (dc[kDcStateControl] & (kWinBActReq | kWinCActReq))) {
        if (frameEnds - scanoutAttachAskedAt == kLatchFrames)
            IOLog("SwitchDisplay: windows B and C did not read off in %u frames of their off request (STATE_CONTROL "
                  "%#x): the scanout domain waits for them\n",
                  kLatchFrames, static_cast<uint32_t>(dc[kDcStateControl]));
        return;
    }
    scanoutAttachWaits = false;
    attachScanoutDomain();
}
