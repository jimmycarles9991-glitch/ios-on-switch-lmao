// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test's window A aperture check (SwitchWindowAperture.h; display.completion_flip winbuf_alias), debug kernels only:
// included in the class body after DisplayWindowProbeDisplay.h. It runs under the gate with window A's completion flip
// withdrawn, so no writer of window A comes between its stores and their restore, and never while a window A request is
// out: LINE_STRIDE is double-buffered (TRM 24.5.4), so a boundary taking that request would latch its test value.

#if SWITCH_FRAME_TIMELINE
// From TegraVIC's user-client thread.
IOReturn windowApertureCall(SwitchDisplayWindowApertureReply &reply)
{
    if (!getWorkLoop())
        return kIOReturnNotReady;
    reply = {};
    reply.magic = SWITCH_DISPLAY_WINDOW_PROBE_MAGIC;
    reply.words = switch_window_aperture::kAllWords;
    windowProbeOnLoop([&] {
        if (!dc || hwSuspended || gPanicRestore.panicking) {
            reply.error = EAGAIN;
            return;
        }
        if (windowProbeBusy() || columnScanBusy() || landscapeOn() || landscapeHoldsKicks()) {
            reply.error = EBUSY;
            return;
        }
        withdrawCompletionFlip();
        if (dc[kDcStateControl] & kWinAActReq) {
            reply.error = EBUSY;
            return;
        }
        const switch_window_aperture::Result result = switch_window_aperture::check(dc);
        reply.aliased = result.aliased;
        reply.restored = result.restored;
    });
    return kIOReturnSuccess;
}
#endif
