// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: the presents NativeLegacyDisplay calls slow (two or more refresh periods after the one before) and the
// cause it charges each to, in its pacing line ("long"), its SwitchFrame lines and the touch interaction line. Times
// are ticks of one monotonic clock, intervals microseconds.
#pragma once
#include <stdint.h>

namespace switch_slow_present {

constexpr uint64_t kGapUs = 250000;  // a longer interval is a screen that went still, or a stall

struct Interval {
    uint32_t periods;  // whole refresh periods, rounded
    bool slow;         // charged to a cause, with a SwitchFrame line
};

// A present intervalUs after the one before. A gap of kGapUs or more is slow only inside a touch window, where it may
// be the stall the touch is waiting on (or a finger at rest: the interaction line counts those as "still").
inline Interval interval(uint64_t intervalUs, uint64_t periodUs, bool touching)
{
    const uint64_t periods = (intervalUs + periodUs / 2) / periodUs;
    const uint32_t whole = periods > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(periods);
    return {whole, whole >= 2 && (intervalUs < kGapUs || touching)};
}

enum Cause : unsigned {
    kIdle,   // the compositor turned vsync off since the previous present: it had nothing to draw (no stall)
    kCa,     // its swap came at or after the boundary a one-frame interval would have used: the compositor was late
    kGpu,    // the swap came in time, but kick to flip alone outlasted the time it had left: its GPU work ended late
    kQueue,  // in time, but waiting behind the previous copy or for a surface made it miss
    kLatch,  // the flip was asked for in time but taken a boundary later
    kRate,   // its swap came under half a period after the vsync it answered: the frames before had nothing new (r17)
    // Its swap came late with no frame end in the two periods before it: FRAME_END was masked (the display idle), so
    // nothing tells a late compositor from one that had nothing to draw until then.
    kMasked,
    kCauses
};

inline constexpr const char *kNames[kCauses] = {"idle", "ca", "gpu", "queue", "latch", "rate", "masked"};

// A present's swap: its submission, its kick and its flip (0: not known), and the vsync it came after, if one came in
// the two periods before its submission (0: none).
struct Swap {
    uint64_t submit, kick, ready, vsync;
};

inline Cause cause(const Swap &s, uint64_t lastPresent, uint64_t period, bool idleSincePresent)
{
    const uint64_t due = lastPresent + period;  // the boundary a one-frame interval would have used
    if (!s.submit || s.submit >= due) {
        if (idleSincePresent)
            return kIdle;
        if (s.vsync && s.submit - s.vsync < period / 2)
            return kRate;
        if (s.submit && !s.vsync)
            return kMasked;
        return kCa;
    }
    if (s.ready > due)
        return s.ready - s.kick > due - s.submit ? kGpu : kQueue;
    return kLatch;
}

}  // namespace switch_slow_present
