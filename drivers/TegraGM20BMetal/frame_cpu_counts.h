// SPDX-License-Identifier: MIT
// Debug builds: what a frame's CPU work met, for the "frame" rates line (TegraGM20BMetalEncoderRates.mm): how the
// recording thread's passes came out of the pass cache, why committed command buffers were not encoded ahead, how long
// queue heads stayed parked and for what, and where a waitUntilScheduled's time went. Counters are summed by the threads
// that meet the events and taken by the rates timer.
#pragma once
#include <atomic>
#include <cstdint>

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
namespace gm20b {

// A render pass as the recording thread opened it
enum class PassOpen : uint8_t {
    Hit,      // a kept shape's copy taken
    Kept,     // made, and kept for the next of its contents
    Unkept,   // made and not kept: it resolves, counted a skip, or failed its command buffer
    Unkeyed,  // made with no key: its descriptor is not of Metal's classes, or names a sample buffer or a foreign object
    Counters,  // made with no key: the process made a counter sample buffer, which turns keying off
    Count
};

// What stopped a committed command buffer's encode ahead besides its vetoes (SwitchMetalPipelineVeto)
enum class AheadLost : uint8_t {
    None,
    Short,  // never handed: fewer than the hand-off's commands were recorded before its commit
    Busy,   // handed while the worker encoded another command buffer ahead
    Late,   // handed, and the worker had not begun it by its commit
};

enum class EnqueueTiming : uint8_t {
    Never,
    BeforeFirstEncoder,
    BeforeCommit,
    Count
};

// A queue head the submit worker parked, by gm20b::HeadWait
constexpr unsigned kHeadWaits = 5;

// One command buffer's schedule as the submit worker saw it (microseconds, nowUs()), read by its waitUntilScheduled
struct ScheduleNotes {
    std::atomic<uint64_t> takenUs{0}, parkedAtUs{0}, parkedUs{0}, encodeUs{0}, scheduledUs{0};
    std::atomic<uint8_t> parkedWhy{0}, aheadLost{0};
};

// Where one waitUntilScheduled's time went
struct ScheduledWait {
    uint64_t queued = 0;    // before the worker first took the command buffer: busy with others, or not yet woken
    uint64_t parked = 0;    // its head parked (compile, event, ...)
    uint64_t encoding = 0;  // the worker encoding and submitting its ranges
    uint64_t handlers = 0;  // from its scheduling to its scheduled handlers having run (the handler queue's hop)
    uint64_t other = 0;     // the rest: readiness checks, other command buffers between its turns

    uint64_t total() const
    {
        return queued + parked + encoding + handlers + other;
    }
};

inline ScheduledWait splitScheduledWait(const ScheduleNotes &n, uint64_t from, uint64_t to)
{
    ScheduledWait w;
    uint64_t left = to > from ? to - from : 0;
    const auto take = [&left](uint64_t want) {
        const uint64_t got = want < left ? want : left;
        left -= got;
        return got;
    };
    const uint64_t taken = n.takenUs.load(std::memory_order_relaxed);
    w.queued = take(taken ? (taken > from ? taken - from : 0) : left);
    w.parked = take(n.parkedUs.load(std::memory_order_relaxed));
    w.encoding = take(n.encodeUs.load(std::memory_order_relaxed));
    const uint64_t scheduled = n.scheduledUs.load(std::memory_order_relaxed);
    w.handlers = take(scheduled ? to - (scheduled > from ? scheduled : from) : 0);
    w.other = left;
    return w;
}

struct FrameCpuCounts {
    std::atomic<uint64_t> passes[size_t(PassOpen::Count)] = {};
    std::atomic<uint64_t> passFull{0};      // misses that found the bank's every entry taken
    std::atomic<uint64_t> passOwned{0};     // hits whose encoder then took a copy of its own (gm20bOwnPass)
    std::atomic<uint64_t> buffers{0};       // command buffers with render work finished
    std::atomic<uint64_t> ahead{0};         // of them, every encoder but the floor's encoded ahead
    std::atomic<uint64_t> vetoes[32] = {};  // by SwitchMetalPipelineVeto bit, the recording's and the worker's
    std::atomic<uint64_t> lost[4] = {};     // by AheadLost
    std::atomic<uint64_t> parks[kHeadWaits] = {}, parkedUs[kHeadWaits] = {};
    std::atomic<uint64_t> waits{0}, waitedUs{0}, waitQueued{0}, waitParked{0}, waitEncoding{0}, waitHandlers{0},
        waitOther{0};
    std::atomic<uint64_t> longestWait{0};
    // the longest wait's parts (ScheduledWait's order), written by the waiter whose wait became the longest
    std::atomic<uint64_t> longestParts[5] = {};
    std::atomic<uint64_t> recordPrepares{0};  // encoder ranges the recording thread prepared (gm20bPrepareEncoder)
    std::atomic<uint64_t> splits{0};          // ranges split where the encode ahead ended (gm20bPipelineSplit)
    std::atomic<uint64_t> rangeSplits{0};
    // from a command buffer's commit to its first raw range's job: over the window and the longest
    std::atomic<uint64_t> kicks{0}, kickUs{0}, kickMaxUs{0};
    std::atomic<uint64_t> enqueues[size_t(EnqueueTiming::Count)] = {};
    std::atomic<uint32_t> lateWritePeriod{128};
    std::atomic<uint64_t> lateWriteSampled{0}, lateWriteBuffer{0}, lateWriteReplace{0}, lateWriteSurface{0};
};

inline FrameCpuCounts &frameCpuCounts()
{
    static FrameCpuCounts &counts = *new FrameCpuCounts;
    return counts;
}

inline void countPass(PassOpen how)
{
    frameCpuCounts().passes[size_t(how)].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace gm20b
#endif
