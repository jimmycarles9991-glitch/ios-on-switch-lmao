// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test display.planes_composite, debug kernels only: TegraVIC's kernel-test selector arms and reports the overlay
// composite's probe (SwitchPlaneCompositeDisplay.h planeCompositeProbe) through this, on the display's work loop.
// Included at NativeLegacyDisplay.cpp's end.
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
extern "C" int switch_display_composite_probe(uint32_t arm, uint32_t *composited)
{
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    IOWorkLoop *loop = display ? display->getWorkLoop() : nullptr;
    if (!loop || !composited)
        return -19;

    struct Call {
        NativeLegacyDisplay *display;
        uint32_t arm, composited;
    } call = {display, arm, 0};

    loop->runAction(
        [](OSObject *, void *argument, void *, void *, void *) -> IOReturn {
            auto *c = static_cast<Call *>(argument);
            c->composited = c->display->planeCompositeProbe(c->arm != 0);
            return kIOReturnSuccess;
        },
        display, &call);
    *composited = call.composited;
    return 0;
}
#endif
