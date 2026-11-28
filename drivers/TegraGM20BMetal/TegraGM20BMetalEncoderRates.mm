// SPDX-License-Identifier: MIT
// Debug builds' "frame" rates line: what a frame's CPU work met in the window (frame_cpu_counts.h)
#import "TegraGM20BMetalPipeline.h"
#include "object_pool.h"
#include "scheduled_wait.h"
#include <cstdarg>

#if SWITCH_METAL_STATS
namespace {

// A wait this long is named on its own line as it ends, with where its time went
constexpr uint64_t kLongWaitUs = 50000;

const char *const kVetoNames[32] = {
    "-",       "counters", "shadows", "variant", "probes", "parallel", "ops",
    "refused", "planar",   "native",  "failed",  "other",  "device",   "channel",
};

const char *const kLostNames[] = {"-", "short", "busy", "late"};
const char *const kWaitNames[gm20b::kHeadWaits] = {"event", "promised", "compile", "signal", "order"};

uint64_t take(std::atomic<uint64_t> &v)
{
    return v.exchange(0, std::memory_order_relaxed);
}

struct Line {
    char *out;
    size_t size, used = 0;
};

__attribute__((format(printf, 2, 3))) void put(Line &line, const char *format, ...)
{
    if (line.used + 1 >= line.size)
        return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(line.out + line.used, line.size - line.used, format, args);
    va_end(args);
    line.used = n < 0 ? line.used : std::min(line.size - 1, line.used + size_t(n));
}

}  // namespace

void gm20bNoteTaken(SwitchMetalCommandBuffer *cb)
{
    gm20b::ScheduleNotes &n = cb->_scheduleNotes;
    const uint64_t now = nowUs();
    uint64_t none = 0;
    n.takenUs.compare_exchange_strong(none, now, std::memory_order_relaxed);
    if (const uint64_t at = n.parkedAtUs.exchange(0, std::memory_order_relaxed)) {
        const uint64_t parked = now - at;
        n.parkedUs.fetch_add(parked, std::memory_order_relaxed);
        const unsigned why = n.parkedWhy.load(std::memory_order_relaxed);
        gm20b::frameCpuCounts().parks[why].fetch_add(1, std::memory_order_relaxed);
        gm20b::frameCpuCounts().parkedUs[why].fetch_add(parked, std::memory_order_relaxed);
    }
}

void gm20bNoteParked(SwitchMetalCommandBuffer *cb, unsigned why)
{
    gm20b::ScheduleNotes &n = cb->_scheduleNotes;
    uint64_t none = 0;
    if (n.parkedAtUs.compare_exchange_strong(none, nowUs(), std::memory_order_relaxed))
        n.parkedWhy.store(uint8_t(why < gm20b::kHeadWaits ? why : 0), std::memory_order_relaxed);
}

void gm20bNoteFinished(SwitchMetalCommandBuffer *cb)
{
    const uint32_t encoders = cb->_pipelineEncoders;
    if (!encoders)
        return;
    gm20b::FrameCpuCounts &c = gm20b::frameCpuCounts();
    c.buffers.fetch_add(1, std::memory_order_relaxed);
    // the last two encoders are encoded at the commit by design (gm20bPublishEncoder)
    if (encoders - std::min(encoders, cb->_pipelinedEncoders) <= 2)
        c.ahead.fetch_add(1, std::memory_order_relaxed);
    for (uint32_t bits = cb->_pipelineVeto.load(std::memory_order_relaxed) | cb->_pipelineGivenBack |
                         cb->_aheadStopped.load(std::memory_order_relaxed);
         bits; bits &= bits - 1)
        c.vetoes[__builtin_ctz(bits)].fetch_add(1, std::memory_order_relaxed);
    const auto lost = cb->_handedOnce ? gm20b::AheadLost(cb->_scheduleNotes.aheadLost.load(std::memory_order_relaxed))
                                      : gm20b::AheadLost::Short;
    if (lost != gm20b::AheadLost::None)
        c.lost[size_t(lost)].fetch_add(1, std::memory_order_relaxed);
    if (cb->_kickedTicks > cb->_committedTicks && cb->_committedTicks) {
        static mach_timebase_info_data_t tb;
        if (!tb.denom)
            mach_timebase_info(&tb);
        const uint64_t us = (cb->_kickedTicks - cb->_committedTicks) * tb.numer / tb.denom / 1000;
        c.kicks.fetch_add(1, std::memory_order_relaxed);
        c.kickUs.fetch_add(us, std::memory_order_relaxed);
        uint64_t longest = c.kickMaxUs.load(std::memory_order_relaxed);
        while (us > longest && !c.kickMaxUs.compare_exchange_weak(longest, us, std::memory_order_relaxed)) {
        }
    }
    const size_t enq = cb->_enqueueTiming < size_t(gm20b::EnqueueTiming::Count) ? cb->_enqueueTiming : 0;
    c.enqueues[enq].fetch_add(1, std::memory_order_relaxed);
}

void gm20bNoteEnqueue(SwitchMetalCommandBuffer *cb)
{
    if (!cb->_enqueueTiming && !cb->_committed.load(std::memory_order_relaxed))
        cb->_enqueueTiming = uint8_t(!cb->_active && cb->_plan.empty() ? gm20b::EnqueueTiming::BeforeFirstEncoder
                                                                       : gm20b::EnqueueTiming::BeforeCommit);
}

void gm20bNoteScheduledWait(SwitchMetalCommandBuffer *cb, uint64_t fromUs, uint64_t toUs)
{
    const gm20b::ScheduledWait w = gm20b::splitScheduledWait(cb->_scheduleNotes, fromUs, toUs);
    gm20b::FrameCpuCounts &c = gm20b::frameCpuCounts();
    c.waits.fetch_add(1, std::memory_order_relaxed);
    c.waitedUs.fetch_add(w.total(), std::memory_order_relaxed);
    c.waitQueued.fetch_add(w.queued, std::memory_order_relaxed);
    c.waitParked.fetch_add(w.parked, std::memory_order_relaxed);
    c.waitEncoding.fetch_add(w.encoding, std::memory_order_relaxed);
    c.waitHandlers.fetch_add(w.handlers, std::memory_order_relaxed);
    c.waitOther.fetch_add(w.other, std::memory_order_relaxed);
    uint64_t longest = c.longestWait.load(std::memory_order_relaxed);
    while (w.total() > longest && !c.longestWait.compare_exchange_weak(longest, w.total(), std::memory_order_relaxed)) {
    }
    if (w.total() > longest) {
        const uint64_t parts[5] = {w.queued, w.parked, w.encoding, w.handlers, w.other};
        for (size_t i = 0; i < 5; i++)
            c.longestParts[i].store(parts[i], std::memory_order_relaxed);
    }
    if (w.total() < kLongWaitUs)
        return;
    const unsigned why = cb->_scheduleNotes.parkedWhy.load(std::memory_order_relaxed);
    os_log(gm20bLog(),
           "SwitchMetal: waitUntilScheduled blocked %llu us on queue %{public}s: before the worker took it %llu, "
           "parked %llu (last for %{public}s), encoding %llu, its scheduled handlers %llu, other %llu",
           (unsigned long long)w.total(), cb->_queue.label.UTF8String ?: "-", (unsigned long long)w.queued,
           (unsigned long long)w.parked, w.parked ? kWaitNames[why < gm20b::kHeadWaits ? why : 0] : "-",
           (unsigned long long)w.encoding, (unsigned long long)w.handlers, (unsigned long long)w.other);
}
#endif

size_t metal_frame_rates(char *out, size_t size)
{
#if SWITCH_METAL_STATS
    if (!gRM || !size)
        return 0;
    gm20b::FrameCpuCounts &c = gm20b::frameCpuCounts();
    uint64_t passes[size_t(gm20b::PassOpen::Count)];
    uint64_t opened = 0;
    for (size_t i = 0; i < size_t(gm20b::PassOpen::Count); i++)
        opened += passes[i] = take(c.passes[i]);
    const uint64_t buffers = take(c.buffers), waits = take(c.waits);
    if (!opened && !buffers && !waits)
        return 0;
    Line line{out, size};
    using P = gm20b::PassOpen;
    put(line, "frame pass=%llu hit=%llu kept=%llu unkept=%llu unkeyed=%llu counters=%llu owned=%llu",
        (unsigned long long)opened, (unsigned long long)passes[size_t(P::Hit)],
        (unsigned long long)passes[size_t(P::Kept)], (unsigned long long)passes[size_t(P::Unkept)],
        (unsigned long long)passes[size_t(P::Unkeyed)], (unsigned long long)passes[size_t(P::Counters)],
        (unsigned long long)take(c.passOwned));
    static uint64_t lastPipelined, lastSealed;
    const uint64_t pipelined = gRM->stats.pipelinedEncoders.load(std::memory_order_relaxed),
                   sealed = gRM->stats.sealedEncoders.load(std::memory_order_relaxed);
    const uint64_t kicks = take(c.kicks), kickUs = take(c.kickUs);
    put(line, " ind=%llu/%llu", (unsigned long long)take(c.indirectDrained), (unsigned long long)take(c.indirectFree));
    put(line, " split=%llu kick=%llu/%lluus", (unsigned long long)take(c.splits),
        (unsigned long long)(kicks ? kickUs / kicks : 0), (unsigned long long)take(c.kickMaxUs));
    // the Shared spans zeroed at the front of a command buffer since the last line (scrubs.cpp), against buffers
    static uint64_t lastScrubRuns, lastScrubBytes;
    const uint64_t scrubRuns = gRM->raw->stats.scrubbedRuns.load(std::memory_order_relaxed),
                   scrubBytes = gRM->raw->stats.scrubbedBytes.load(std::memory_order_relaxed);
    put(line, " scrub=%llu/%lluKiB", (unsigned long long)(scrubRuns - lastScrubRuns),
        (unsigned long long)((scrubBytes - lastScrubBytes) >> 10));
    lastScrubRuns = scrubRuns;
    lastScrubBytes = scrubBytes;
    put(line, " ahead=%llu/%llu enc=%llu/%llu why=", (unsigned long long)take(c.ahead), (unsigned long long)buffers,
        (unsigned long long)(pipelined - lastPipelined), (unsigned long long)(sealed - lastSealed));
    lastPipelined = pipelined;
    lastSealed = sealed;
    bool any = false;
    for (unsigned bit = 0; bit < 32; bit++)
        if (const uint64_t n = take(c.vetoes[bit])) {
            put(line, "%s%s:%llu", any ? "," : "", kVetoNames[bit] ? kVetoNames[bit] : "?", (unsigned long long)n);
            any = true;
        }
    for (unsigned k = 1; k < 4; k++)
        if (const uint64_t n = take(c.lost[k])) {
            put(line, "%s%s:%llu", any ? "," : "", kLostNames[k], (unsigned long long)n);
            any = true;
        }
    if (!any)
        put(line, "-");
    const Class pooled[] = {[SwitchMetalRenderEncoder class], [SwitchMetalCommandBuffer class]};
    for (Class cls : pooled)
        put(line, " %s=%llu/%llu/%u/%u", cls == [SwitchMetalCommandBuffer class] ? "cbs" : "encoders",
            (unsigned long long)gm20bPoolMade(cls), (unsigned long long)gm20bPoolFreed(cls), gm20bPoolMost(cls),
            gm20bPoolKept(cls));
    put(line, " park=");
    any = false;
    for (unsigned k = 0; k < gm20b::kHeadWaits; k++)
        if (const uint64_t n = take(c.parks[k])) {
            put(line, "%s%s:%llu/%lluus", any ? "," : "", kWaitNames[k], (unsigned long long)n,
                (unsigned long long)take(c.parkedUs[k]));
            any = true;
        }
    if (!any)
        put(line, "-");
    put(line, " ws=%llu/%lluus(queued %llu parked %llu encoding %llu handlers %llu other %llu)",
        (unsigned long long)waits, (unsigned long long)take(c.waitedUs), (unsigned long long)take(c.waitQueued),
        (unsigned long long)take(c.waitParked), (unsigned long long)take(c.waitEncoding),
        (unsigned long long)take(c.waitHandlers), (unsigned long long)take(c.waitOther));
    put(line, " wsmax=%lluus(%llu %llu %llu %llu %llu)", (unsigned long long)take(c.longestWait),
        (unsigned long long)take(c.longestParts[0]), (unsigned long long)take(c.longestParts[1]),
        (unsigned long long)take(c.longestParts[2]), (unsigned long long)take(c.longestParts[3]),
        (unsigned long long)take(c.longestParts[4]));
    put(line, " early=");
    for (size_t k = 0; k < size_t(gm20b::EarlyOutcome::Count); k++)
        put(line, "%s%llu", k ? "/" : "", (unsigned long long)take(c.early[k]));
    using E = gm20b::EnqueueTiming;
    put(line, " enq=%llu/%llu/%llu", (unsigned long long)take(c.enqueues[size_t(E::BeforeFirstEncoder)]),
        (unsigned long long)take(c.enqueues[size_t(E::BeforeCommit)]),
        (unsigned long long)take(c.enqueues[size_t(E::Never)]));
    return line.used;
#else
    (void)out;
    (void)size;
    return 0;
#endif
}
