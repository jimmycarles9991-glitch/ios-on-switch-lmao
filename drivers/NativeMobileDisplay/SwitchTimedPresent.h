// SPDX-License-Identifier: BSD-2-Clause-Patent
// When a swap's requested presentation lets it go. Core Animation stamps every swap with the vsync it rendered for
// (swapRequestedPresentation, mach continuous time); presentAtTime: and presentAfterMinimumDuration: move that later.
// DC A latches a flip only at a frame end, so a flip armed now shows from the next frame end at the earliest: a swap
// waits while that frame end is more than half a period before its target.
//
// The render server's frame clock is phased by frame info's Presentation_time (the frame end), so its stamp names a
// frame end and the stamp alone decides. In some windows it stamps the second frame end ahead: shown at once when
// accepted late in the period and held when accepted early, consecutive frames would latch two periods apart. With the
// frame ends masked there is no phase to judge the stamp by: a swap stamped no more than a period and a half after it
// was accepted is the render server's frame for its next vsync and goes at once (OrdinaryUnphased); a later target
// waits for the next frame end. The debug counters (kern.switch_frame_info_unphased, _early and the "SwitchDisplay
// requested" line) say how often.
#pragma once
#include <stdint.h>

namespace switch_timed_present {

struct Clock {
    uint64_t nowAbsolute, nowContinuous;
    uint64_t lastFrameEnd;  // mach absolute time of the last DC A frame end the filter saw
    uint64_t period;        // the frame period, mach absolute units
    uint64_t horizon;       // a target further out than this is not one the render server holds for: shown as now
    uint64_t accepted;      // mach absolute time the swap was accepted (0: unknown)
    // 21H461: a phased swap within 1.5 periods of its acceptance goes at once (Ordinary) whatever its stamp; no 21H461
    // capture shows its render server's timer phased to the frame end yet (devicerun display.timed-ordinary-17).
    bool phasedOrdinary;
};

enum class Decision {
    Due,
    Early,             // the next frame end is too early for it
    NeedsPhase,        // the frame ends were masked: the next one says which frame end is the target's
    Ordinary,          // phasedOrdinary: too early for the next frame end, and goes at once as an ordinary frame
    OrdinaryUnphased,  // the frame ends were masked, and it goes at once as the render server's ordinary frame
};

inline bool due(Decision d)
{
    return d == Decision::Due || d == Decision::Ordinary || d == Decision::OrdinaryUnphased;
}

inline Decision decide(uint64_t requestedContinuous, const Clock &c)
{
    if (!requestedContinuous || !c.period || c.nowContinuous < c.nowAbsolute)
        return Decision::Due;
    const uint64_t asleep = c.nowContinuous - c.nowAbsolute;
    if (requestedContinuous <= asleep)
        return Decision::Due;
    const uint64_t requested = requestedContinuous - asleep;
    if (requested <= c.nowAbsolute + c.period / 2 || requested - c.nowAbsolute > c.horizon)
        return Decision::Due;
    // A frame end the filter stamped after nowAbsolute was read is now.
    const uint64_t lastFrameEnd = c.lastFrameEnd > c.nowAbsolute ? c.nowAbsolute : c.lastFrameEnd;
    // Unphased (24B119), only a stamp for the next vsync counts as ordinary: a period, and the render thread's wake
    // jitter (250 us, under P/16). A second-ahead stamp waits a frame end for the phase, at no cost to it.
    const uint64_t span = c.period + (c.phasedOrdinary ? c.period / 2 : c.period / 16);
    const bool ordinary = c.accepted && requested <= c.accepted + span;
    if (!lastFrameEnd || c.nowAbsolute - lastFrameEnd >= 2 * c.period)
        return ordinary ? Decision::OrdinaryUnphased : Decision::NeedsPhase;
    const uint64_t nextFrameEnd = lastFrameEnd + ((c.nowAbsolute - lastFrameEnd) / c.period + 1) * c.period;
    if (requested <= nextFrameEnd + c.period / 2)
        return Decision::Due;
    return ordinary && c.phasedOrdinary ? Decision::Ordinary : Decision::Early;
}

}  // namespace switch_timed_present
