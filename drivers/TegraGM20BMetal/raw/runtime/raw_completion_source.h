// SPDX-License-Identifier: MIT
// One completion source per device: the completion thread sleeps in the kernel on one job at a time, the first in flight
// whose end raises an interrupt, and retires every range before it when it wakes. Every other job ends quiet
// (TegraGM20BJobQuiet): its range is covered by a later job of the device's channel, which ends after it.
#pragma once
#include <cstddef>
#include <optional>

namespace raw {

// What a raw range's closing job knows when it is submitted
struct RangeEnd {
    bool lastOfBuffer = true;   // the command buffer's last range
    bool nextRangeRaw = false;  // not the last: its next range is raw work the submit thread takes next
    bool followed = false;      // the last: another committed command buffer is ready behind it
    bool signals = false;       // shared event signals the CPU publishes when the range completes
    bool observed = false;      // completed handlers, a drawable present, or a thread in waitUntilCompleted
    bool cpuOrdered = false;    // m2v work waits for raw completions on the CPU (the kernel refused Order)
};

// Quiet when nothing waits for this range alone and the submit thread has more raw work in hand to put behind it
inline bool quietEnd(const RangeEnd &end)
{
    if (end.signals || end.cpuOrdered)
        return false;
    return end.lastOfBuffer ? !end.observed && end.followed : end.nextRangeRaw;
}

// The in-flight range the completion thread sleeps on: the first whose job interrupts. With only quiet ranges in flight,
// the newest once the submit thread has promised nothing behind them (it went idle or parked) or a thread waits for one
// of them; the kernel then pushes a wake entry behind it. nullopt: the job that covers them is still to be submitted.
template <typename Ranges> std::optional<size_t> completionTarget(const Ranges &inflight, bool promised, bool waited)
{
    size_t ranges = 0;
    for (const auto &range : inflight) {
        if (!range.quiet)
            return ranges;
        ++ranges;
    }
    if (ranges && (!promised || waited))
        return ranges - 1;
    return std::nullopt;
}

}  // namespace raw
