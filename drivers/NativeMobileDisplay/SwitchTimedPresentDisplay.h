// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kick's timed-present gate (SwitchTimedPresent.h): a swap whose target is later than the frame end its flip would
// latch at stays queued, and the frame-end interrupt (kept unmasked while kickDeferred is set, kickAtIdleFrameEnd) kicks
// it again at each frame end until the next one is its target's. No CPU timer. Included in the class body after
// SwitchRetireDisplay.h; under the gate.

static constexpr uint64_t kTimedPresentHorizonNs = 1000000000ULL;
#if SWITCH_IPSW_KIND_24B119
static constexpr bool kPhasedOrdinary = false;  // 24B119's render server stamps the frame end it wants
#else
static constexpr bool kPhasedOrdinary = true;
#endif
// The kick's last answer from the gate: the head waits only for its requested presentation (kickAtIdleFrameEnd).
static inline bool headHeldForTime = false;
// The swap the gate holds, for frame info's count of timed swaps. Static: the instance is at the 16 KiB a kalloc_type
// class may have.
static inline const OriginalLegacySwapRequest *timedHeldRequest = nullptr;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
bool timedHoldLogged = false;
#endif
#if SWITCH_FRAME_TIMELINE
// The swap the gate's last decision let go by the ordinary-frame clause with the frame ends masked: counted once, when
// the kick takes it (a kick that defers it for another reason asks the gate again).
static inline const OriginalLegacySwapRequest *unphasedRequest = nullptr;
#endif

void noteUnphasedTaken(const OriginalLegacySwapRequest *request)
{
#if SWITCH_FRAME_TIMELINE
    if (request && request == unphasedRequest) {
        unphasedRequest = nullptr;
        ++frameInfo->pending.timedUnphased;
    }
#else
    (void)request;
#endif
}

bool presentNotDue(const OriginalLegacySwapRequest *request)
{
    uint64_t requested = 0;
    memcpy(&requested, reinterpret_cast<const unsigned char *>(request) + swapRequestedPresentation, sizeof(requested));
    uint64_t horizon = 0;
    nanoseconds_to_absolutetime(kTimedPresentHorizonNs, &horizon);
    const uint64_t lastFrameEnd = __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE);
    const uint64_t nowAbsolute = mach_absolute_time();
    const uint64_t nowContinuous = mach_continuous_time();
    const switch_timed_present::Clock clock = {nowAbsolute, nowContinuous,        lastFrameEnd,   vsyncInterval,
                                               horizon,     submittedAt(request), kPhasedOrdinary};
    const auto decision = switch_timed_present::decide(requested, clock);
    headHeldForTime = !switch_timed_present::due(decision);
#if SWITCH_FRAME_TIMELINE
    unphasedRequest = decision == switch_timed_present::Decision::OrdinaryUnphased ? request : nullptr;
#endif
    if (switch_timed_present::due(decision))
        return false;
#if SWITCH_FRAME_TIMELINE
    if (request != timedHeldRequest)
        ++(decision == switch_timed_present::Decision::Early ? frameInfo->pending.heldEarly
                                                             : frameInfo->pending.heldPhase);
#endif
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
// gate held for their presentation time whose frame info went out, and those replaced before any frame showed them;
// _unphased, the swaps it let go as ordinary frames with the frame ends masked; _early, the swaps shown more than half a
// period before their requested presentation.
#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kFrameInfoTimedOids = 4;
static inline struct sysctl_oid frameInfoTimedOids[kFrameInfoTimedOids] = {};

static int frameInfoTimedSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    int value = 0;
    const NativeLegacyDisplay *display = __atomic_load_n(&primaryDisplay, __ATOMIC_ACQUIRE);
    if (display && display->frameInfo) {
        const auto &pending = display->frameInfo->pending;
        const uint32_t *const counts[kFrameInfoTimedOids] = {&pending.timedShown, &pending.timedUnsent,
                                                             &pending.shownEarly, &pending.timedUnphased};
        if (which >= 0 && static_cast<unsigned>(which) < kFrameInfoTimedOids)
            value = static_cast<int>(__atomic_load_n(counts[which], __ATOMIC_RELAXED));
    }
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void frameInfoTimedRegister()
{
    static const char *const names[kFrameInfoTimedOids] = {"switch_frame_info_shown", "switch_frame_info_unsent",
                                                           "switch_frame_info_early", "switch_frame_info_unphased"};
    for (unsigned i = 0; i < kFrameInfoTimedOids; ++i) {
        if (frameInfoTimedOids[i].oid_name)
            continue;
        switch_sysctl_oid(&frameInfoTimedOids[i], &sysctl__kern_children, names[i],
                          static_cast<int>(CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED), frameInfoTimedSysctl, "I",
                          "swaps since boot by how their requested presentation went");
        frameInfoTimedOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&frameInfoTimedOids[i]);
    }
}
#else
static void frameInfoTimedRegister()
{}
#endif
