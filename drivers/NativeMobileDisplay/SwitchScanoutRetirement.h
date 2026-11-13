// SPDX-License-Identifier: BSD-2-Clause-Patent
bool scanoutQuiescing = false;
uint64_t scanoutStopFrames = 0;

bool scanoutStorageRetired()
{
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    dc[kDcWindowHeader] = kWindowASelect;
    const bool stopped = !(dc[kDcDisplayCommand] & kDcModeMask);
    const bool pitch = dc[kWinbufStartAddr] == surfaceAddress(0) && dc[kWinOptions] == pitchOptions &&
                       dc[kWinLineStride] == pitchStride && dc[kWinbufSurfaceKind] == pitchKind &&
                       dc[kWinbufHOffset] == pitchHOffset;
    const bool overlays = !(dc[kWinBBase + native_planes::kOptions] & native_planes::kWinEnable) &&
                          !(dc[kWinCBase + native_planes::kOptions] & native_planes::kWinEnable);
    dc[kDcStateAccess] = access;
    return (scanoutStopped && stopped) ||
           (pitch && overlays && !(dc[kDcStateControl] & (kWinAActReq | kWinBActReq | kWinCActReq)));
}

void retireScanoutStorage()
{
    if (hwSuspended)
        panic("SwitchDisplay: cannot retire scanout storage while DC is powered off");
    __atomic_store_n(&scanoutQuiescing, true, __ATOMIC_RELEASE);
    restorePitchWindow();
    dc[kWinBBase + native_planes::kOptions] = 0;
    dc[kWinCBase + native_planes::kOptions] = 0;
    scanoutMemoryBarrier();
    dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate | kWinBUpdate | kWinCUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq;
    unmaskFrameEnd();
    uint64_t deadline = 0;
    clock_interval_to_deadline(kFlipDeadlineMS, kMillisecondScale, &deadline);
    while (!scanoutStorageRetired()) {
        const uint64_t seen = __atomic_load_n(&scanoutStopFrames, __ATOMIC_ACQUIRE);
        assert_wait_deadline(reinterpret_cast<event_t>(&scanoutStopFrames), THREAD_UNINT, deadline);
        // The filter remains live while this thread holds the gate, including between the first check and wait registration.
        if (scanoutStorageRetired() || __atomic_load_n(&scanoutStopFrames, __ATOMIC_ACQUIRE) != seen) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            continue;
        }
        if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT && !scanoutStorageRetired())
            panic("SwitchDisplay: scanout storage remains active after %u ms", kFlipDeadlineMS);
    }
    __atomic_store_n(&scanoutQuiescing, false, __ATOMIC_RELEASE);
}
