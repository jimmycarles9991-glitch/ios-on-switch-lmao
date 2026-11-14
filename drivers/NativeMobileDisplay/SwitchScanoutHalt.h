// SPDX-License-Identifier: BSD-2-Clause-Patent
// SC7's wait for DC A's STOP: in NativeLegacyDisplay's class body after SwitchScanoutRetirement.h, whose frame-end
// wakeup (scanoutQuiescing, scanoutStopFrames) it shares.

// DISPLAY_COMMAND's active copy (TRM 24.6.16 DC_CMD_DISPLAY_COMMAND_0, DISPLAY_CTRL_MODE): STOP is in force once it reads 0.
bool scanoutHalted()
{
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    const bool halted = !(dc[kDcDisplayCommand] & kDcModeMask);
    dc[kDcStateAccess] = access;
    return halted;
}

// SC7, under the gate, after the STOP request: STOP takes effect at the next frame boundary, which ends the frame DC A was
// scanning (TRM 24.6.16), and that frame's FRAME_END wakes this wait. Bounded by two frame periods; past them one error
// line, and the power-down goes on.
void waitScanoutHalted()
{
    if (scanoutHalted())
        return;
    uint64_t period = vsyncInterval;
    if (!period)
        nanoseconds_to_absolutetime(16666667ULL, &period);
    const uint64_t asked = mach_absolute_time(), deadline = asked + 2 * period;
    __atomic_store_n(&scanoutQuiescing, true, __ATOMIC_RELEASE);
    writeIntMask(0, kFrameEndInt);
    bool halted = false;
    for (;;) {
        const uint64_t seen = __atomic_load_n(&scanoutStopFrames, __ATOMIC_ACQUIRE);
        if ((halted = scanoutHalted()))
            break;
        assert_wait_deadline(reinterpret_cast<event_t>(&scanoutStopFrames), THREAD_UNINT, deadline);
        if (__atomic_load_n(&scanoutStopFrames, __ATOMIC_ACQUIRE) != seen) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            continue;
        }
        if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT) {
            halted = scanoutHalted();
            break;
        }
    }
    writeIntMask(kFrameEndInt, 0);
    __atomic_store_n(&scanoutQuiescing, false, __ATOMIC_RELEASE);
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    if (halted)
        IOLog("SwitchDisplay: DC A stopped %llu us after the SC7 stop\n",
              static_cast<unsigned long long>(Span::ticksToUs(mach_absolute_time() - asked)));
#endif
    if (!halted)
        IOLog("SwitchDisplay: DC A still scanning %llu us after the SC7 stop (DISPLAY_COMMAND active %#x, INT_STATUS "
              "%#x): it is powered down now\n",
              static_cast<unsigned long long>(Span::ticksToUs(mach_absolute_time() - asked)),
              static_cast<uint32_t>(dc[kDcDisplayCommand]), static_cast<uint32_t>(dc[kDcIntStatus]));
}
