// SPDX-License-Identifier: MIT
// One completion source per device: the completion thread sleeps in the kernel on one job at a time, the first in flight
// whose end raises an interrupt, and retires every range before it when it wakes. Every other job ends quiet
// (TegraGM20BJobQuiet): its range is covered by a later job of the device's channel, which ends after it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace gm20b {

// What a raw range's closing job knows when it is submitted
struct RangeEnd {
    bool lastOfBuffer = true;   // the command buffer's last range
    bool nextRangeRaw = false;  // not the last: its next range is raw work the submit thread takes next
    bool followed = false;      // the last: another committed command buffer is ready behind it
    bool signals = false;       // shared event signals the CPU publishes when the range completes
    bool observed = false;      // completed handlers, a drawable present, or a thread in waitUntilCompleted
    bool handlersOnly = false;  // observed by completed handlers alone
    bool compositor = false;    // the render server's device (gm20b::Device::Role::RenderServer)
};

// Quiet when nothing waits for this range alone and the submit thread has more raw work in hand to put behind it. The
// compositor's command buffers whose completed handlers alone observe them end quiet with nothing behind: Core Animation
// commits a frame's buffers one at a time (MetalContext::flush), and the frame ends loud: the scanout's last writer (the
// kernel keeps it loud, TegraGM20BExecReplyLoud), the present's ask for it, or a waiter. Their handlers run from that
// completion, in order.
inline bool quietEnd(const RangeEnd &end)
{
    if (end.signals)
        return false;
    if (!end.lastOfBuffer)
        return end.nextRangeRaw;
    if (end.compositor && (!end.observed || end.handlersOnly))
        return true;
    return !end.observed && end.followed;
}

// A quiet range nothing in hand follows, left for its frame's loud end
inline bool frameHeld(const RangeEnd &end)
{
    return end.compositor && end.lastOfBuffer && !end.followed && quietEnd(end);
}

// Core Animation commits a frame's command buffers within its display period: one period after a held range, no later
// commit of its frame is coming, and the completion thread asks for the range's own interrupt.
constexpr uint64_t kFrameEndNs = 16666667;

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

// completionTarget for ranges a frame holds (frameDue: 0, or when its frame's end is past due): with only quiet ranges in
// flight and the newest held, nothing is waited on until its due time unless a thread waits for one; heldUntil is when
// to look again.
struct CompletionLook {
    std::optional<size_t> target;
    uint64_t heldUntil = 0;
};

template <typename Ranges>
CompletionLook completionLook(const Ranges &inflight, bool promised, bool waited, uint64_t now)
{
    size_t ranges = 0;
    uint64_t due = 0;
    for (const auto &range : inflight) {
        if (!range.quiet)
            return {ranges, 0};
        due = range.frameDue;
        ++ranges;
    }
    if (!ranges || (promised && !waited))
        return {};
    if (!waited && due > now)
        return {std::nullopt, due};
    return {ranges - 1, 0};
}

}  // namespace gm20b
