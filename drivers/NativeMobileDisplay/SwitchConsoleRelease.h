// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU's data-migration hold release (kern_exec.c native_migration_tick, a thread call; both kernels): the display shows
// the compositor's current frame now instead of at its next swap (SwitchConsoleReleaseDisplay.h). Included at the end of
// NativeLegacyDisplay.cpp.

extern "C" void native_legacy_console_released(void)
{
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    IOWorkLoop *loop = display ? display->getWorkLoop() : nullptr;
    if (!loop)
        return;
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->consoleReleasedKick();
            return kIOReturnSuccess;
        },
        display);
}
