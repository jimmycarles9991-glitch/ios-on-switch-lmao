// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kick's timed-present gate (SwitchTimedPresent.h): a swap whose target is later than the frame end its flip would
// latch at stays queued, and the frame-end interrupt (kept unmasked while kickDeferred is set, kickAtIdleFrameEnd) kicks
// it again at each frame end until the next one is its target's. No CPU timer. Included in the class body after
// SwitchRetireDisplay.h; under the gate.

static constexpr uint64_t kTimedPresentHorizonNs = 1000000000ULL;
// The swap the gate holds, for frame info's count of timed swaps. Static: the instance is at the 16 KiB a kalloc_type
// class may have.
static inline const OriginalLegacySwapRequest *timedHeldRequest = nullptr;
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
    timedHeldRequest = request;
    kickDeferred = true;
    return true;
}

// display.present_at_time's view of frame info (debug kernels): kern.switch_frame_info_shown and _unsent, the swaps the
// gate held for their presentation time whose frame info went out, and those replaced before any frame showed them.
#if SWITCH_FRAME_TIMELINE
static inline struct sysctl_oid frameInfoTimedOids[2] = {};

static int frameInfoTimedSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    int value = 0;
    const NativeLegacyDisplay *display = __atomic_load_n(&primaryDisplay, __ATOMIC_ACQUIRE);
    if (display && display->frameInfo) {
        const auto &pending = display->frameInfo->pending;
        value = static_cast<int>(which ? __atomic_load_n(&pending.timedUnsent, __ATOMIC_RELAXED)
                                       : __atomic_load_n(&pending.timedShown, __ATOMIC_RELAXED));
    }
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void frameInfoTimedRegister()
{
    static const char *const names[2] = {"switch_frame_info_shown", "switch_frame_info_unsent"};
    for (unsigned i = 0; i < 2; ++i) {
        if (frameInfoTimedOids[i].oid_name)
            continue;
        switch_sysctl_oid(&frameInfoTimedOids[i], &sysctl__kern_children, names[i],
                          static_cast<int>(CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED), frameInfoTimedSysctl, "I",
                          "swaps held for their presentation time since boot: frame info sent, replaced unsent");
        frameInfoTimedOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&frameInfoTimedOids[i]);
    }
}
#else
static void frameInfoTimedRegister()
{}
#endif
