// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kick's timed-present gate (SwitchTimedPresent.h): a swap whose target is later than the frame end its flip would
// latch at stays queued, and the frame-end interrupt (kept unmasked while kickDeferred is set, kickAtIdleFrameEnd) kicks
// it again at each frame end until the next one is its target's. No CPU timer. Included in the class body after
// SwitchRetireDisplay.h; under the gate.

static constexpr uint64_t kTimedPresentHorizonNs = 1000000000ULL;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
bool timedHoldLogged = false;
#endif

bool presentNotDue(const OriginalLegacySwapRequest *request)
{
    uint64_t requested = 0;
    memcpy(&requested, reinterpret_cast<const unsigned char *>(request) + swapRequestedPresentation, sizeof(requested));
    uint64_t horizon = 0;
    nanoseconds_to_absolutetime(kTimedPresentHorizonNs, &horizon);
    const switch_timed_present::Clock clock = {mach_absolute_time(),
                                               mach_continuous_time(),
                                               __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE),
                                               vsyncInterval,
                                               horizon,
                                               submittedAt(request)};
    const auto decision = switch_timed_present::decide(requested, clock);
    if (decision == switch_timed_present::Decision::Due)
        return false;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    if (!timedHoldLogged && decision == switch_timed_present::Decision::Early) {
        timedHoldLogged = true;
        uint64_t ahead = 0;
        absolutetime_to_nanoseconds(requested - (clock.nowContinuous - clock.nowAbsolute) - clock.nowAbsolute, &ahead);
        IOLog("SwitchDisplay: first swap held for its presentation time, %llu us ahead: it flips at the frame end "
              "before it\n",
              ahead / 1000);
    }
#endif
    kickDeferred = true;
    return true;
}
