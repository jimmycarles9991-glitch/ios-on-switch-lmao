// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalPipeline.h"
#import "TegraGM20BMetalRangeEncode.h"
#include <CoreFoundation/CoreFoundation.h>

namespace {

// Tape commands recorded since the worker was last handed a command buffer before it is handed again: each hand-off may
// wake the worker, and what was not handed is left for the commit to encode
constexpr uint32_t kHandOff = 24;

// The command buffer encoded ahead (the worker's alone): handed and not begun, or open on the device. The worker gives it
// back or takes it before it finishes that command buffer (gm20bPipelineYield, gm20bPipelineFailed), and before it runs a move
// of a texture's memory (gm20bRunOnSubmitWorker), so no encode ahead spans one: once its completion is sent, nothing of it
// is the worker's, and the counts giveBack wrote on it are final.
struct Ahead {
    SwitchMetalCommandBuffer *cb = nil;
    bool open = false;
    const char *why = nullptr;
};

Ahead &ahead()
{
    static Ahead *a = new Ahead;
    return *a;
}

#if SWITCH_METAL_DIAGNOSTICS
uint32_t encodersBefore(SwitchMetalCommandBuffer *cb, size_t position)
{
    uint32_t n = 0;
    for (size_t at = 0; at < position; ++at)
        n += SwitchMetalCommandPlan::isEncoderBegin(cb->_plan.operation(at).op);
    return n;
}
#endif

// Debug builds: `cb`'s waiters for the worker (switchMetalWaitAhead) woken, after what they read was stored
void aheadChanged(SwitchMetalCommandBuffer *cb)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    gm20bChanged(cb);
#else
    (void)cb;
#endif
}

// The encode ahead given back for `veto` (the first reason counted): the device's command buffer abandoned, and the
// command buffer encoded at its commit
void giveBack(uint32_t veto)
{
    Ahead &a = ahead();
    SwitchMetalCommandBuffer *cb = a.cb;
    if (!cb)
        return;
    if (a.open) {
        gm20bRangeEncode().reset();
        gRM->committed.fetch_sub(1, std::memory_order_release);
        cb->_pipelineAbandons++;
        cb->_pipelineGivenBack |= veto;
        SWITCH_METAL_DIAGNOSTIC(gRM->stats.pipelineAbandons[__builtin_ctz(veto ? veto : uint32_t(VetoFailed))]++);
    }
    cb->_pipelineDeclined.store(true, std::memory_order_release);
    a.cb = nil;
    a.open = false;
    aheadChanged(cb);
}

// The first command the encode ahead of a.cb may not reach: the recording's stop or the one the worker halted at
size_t stopOf(const Ahead &a)
{
    size_t stop = a.cb->_aheadStop.load(std::memory_order_acquire) ?: SIZE_MAX;
    if (a.open)
        if (const size_t halt = gm20bRangeEncode().halted())
            stop = std::min(stop, halt);
    return stop;
}

}  // namespace

bool gm20bAheadReads(__unsafe_unretained SwitchMetalCommandBuffer *cb)
{
    const size_t stop = cb->_aheadStop.load(std::memory_order_relaxed);
    return cb->_handedOnce && !cb->_pipelineVeto.load(std::memory_order_relaxed) &&
           !cb->_pipelineDeclined.load(std::memory_order_relaxed) && (!stop || cb->_encAt < stop);
}

void gm20bPrepareEncoder(__unsafe_unretained SwitchMetalCommandBuffer *cb, size_t from, size_t to)
{
    cb->_plan.prepareRange(from, to);
    SWITCH_METAL_STAT(gm20b::frameCpuCounts().recordPrepares.fetch_add(1, std::memory_order_relaxed));
}

// Before the first hand-off: the encoders recorded so far, which the worker encodes from the tape's start
static void prepareEnded(__unsafe_unretained SwitchMetalCommandBuffer *cb, size_t length)
{
    if (const size_t stop = cb->_aheadStop.load(std::memory_order_relaxed))
        length = std::min(length, stop);
    for (size_t at = 0; at < length;) {
        if (!SwitchMetalCommandPlan::isEncoderBegin(cb->_plan.operation(at).op)) {
            ++at;
            continue;
        }
        const size_t end = cb->_plan.encoderEnd(at);
        if (end <= at || end > length)
            break;
        gm20bPrepareEncoder(cb, at, end);
        at = end;
    }
}

void gm20bPublishEncoder(__unsafe_unretained SwitchMetalCommandBuffer *cb)
{
    const size_t length = cb->_plan.size();
    const uint64_t published = uint64_t(cb->_endedAt) << 32 | length;
    cb->_endedAt = cb->_encAt;
    cb->_handedCommands += uint32_t(length - cb->_encAt);
    if (cb->_handedCommands < kHandOff || cb->_pipelineVeto.load(std::memory_order_relaxed)) {
        cb->_published.store(published, std::memory_order_release);
        return;
    }
    cb->_handedCommands = 0;
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.handoffs++);
    if (!cb->_handedOnce) {
        prepareEnded(cb, length);
        cb->_handedOnce = true;
        cb->_published.store(published, std::memory_order_release);
        SWITCH_METAL_DIAGNOSTIC(gRM->stats.handoffWakes += gRM->submitAsleep.load(std::memory_order_relaxed));
        gRM->schedule.push({SwitchMetalScheduleChange::Encoded, cb});
        return;
    }
    // Dekker pair with submitLoop's submitAsleep.store(seq_cst) -> _published.load(seq_cst)
    cb->_published.store(published, std::memory_order_seq_cst);
    if (!cb->_pipelineDeclined.load(std::memory_order_relaxed) && gRM->submitAsleep.load(std::memory_order_seq_cst)) {
        SWITCH_METAL_DIAGNOSTIC(gRM->stats.handoffWakes++);
        gRM->schedule.wake();
    }
}

void gm20bPipelineHanded(SwitchMetalCommandBuffer *cb)
{
    Ahead &a = ahead();
    if (cb->_pipelineDeclined.load(std::memory_order_relaxed) || cb->_committed.load(std::memory_order_acquire))
        return;
    if (a.cb) {
        SWITCH_METAL_STAT(
            cb->_scheduleNotes.aheadLost.store(uint8_t(gm20b::AheadLost::Busy), std::memory_order_relaxed));
        cb->_pipelineDeclined.store(true, std::memory_order_release);
        aheadChanged(cb);
        return;
    }
    a.cb = cb;
    a.open = false;
}

bool gm20bPipelinePublished()
{
    const Ahead &a = ahead();
    if (!a.cb)
        return false;
    const size_t published = size_t(a.cb->_published.load(std::memory_order_seq_cst) >> 32);
    const size_t until = std::min(published, stopOf(a));
    return until > (a.open ? gm20bRangeEncode().position() : 0);
}

bool gm20bPipelineStep()
{
    Ahead &a = ahead();
    SwitchMetalCommandBuffer *cb = a.cb;
    if (!cb)
        return false;
    const uint64_t published = cb->_published.load(std::memory_order_acquire);
    if (const uint32_t veto = cb->_pipelineVeto.load(std::memory_order_acquire)) {
        giveBack(veto);
        return true;
    }
    const size_t stop = stopOf(a);
    const size_t until = std::min(size_t(published >> 32), stop);
    SwitchMetalRangeEncode &encode = gm20bRangeEncode();
    if (a.open && encode.position() >= until)
        return false;
    if (!a.open && !until)
        return false;
    return gm20bSubmission([&] {
        if (!a.open) {
            gm20bMoveWanting();
            // counted as committed, no purge takes an arena it encodes (gpu.h purgeArenas)
            gRM->committed.fetch_add(1);
            if (gRM->raw->purgedArenas()) {
                encode.reset();
                gRM->committed.fetch_sub(1, std::memory_order_release);
                giveBack(VetoDevice);
                return true;
            }
            // an open() that stops (VetoChannel: the kernel gave it no channel) is given back as an encode that began,
            // its reason counted; the command buffer's own encode at its turn asks for the channel again
            a.open = true;
            if (!encode.open(cb, 0, SIZE_MAX, false, &a.why)) {
                giveBack(encode.vetoed());
                return true;
            }
        }
        encode.limit(stop, size_t(uint32_t(published)));
        if (!encode.step(until)) {
            giveBack(encode.vetoed());
            return true;
        }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        cb->_aheadHalt.store(encode.halted(), std::memory_order_release);
        cb->_aheadAt.store(encode.position(), std::memory_order_release);
        aheadChanged(cb);
#endif
        return true;
    });
}

void gm20bPipelineYield(SwitchMetalCommandBuffer *cb, size_t from, bool nativeRange)
{
    Ahead &a = ahead();
    if (a.cb && !(a.cb == cb && nativeRange && from == 0))
        giveBack(VetoOtherWork);
}

bool gm20bPipelineOpen()
{
    return ahead().open;
}

bool gm20bPipelineHasOpenAhead(SwitchMetalCommandBuffer *cb)
{
    const Ahead &a = ahead();
    return a.cb == cb && a.open;
}

void gm20bPipelineFailed(SwitchMetalCommandBuffer *cb)
{
    if (ahead().cb == cb)
        giveBack(cb->_pipelineVeto.load(std::memory_order_acquire) ?: uint32_t(VetoFailed));
}

bool gm20bPipelineTake(SwitchMetalCommandBuffer *cb, size_t from)
{
    Ahead &a = ahead();
    if (a.cb != cb || from)
        return false;
    if (!a.open) {
        SWITCH_METAL_STAT(
            cb->_scheduleNotes.aheadLost.store(uint8_t(gm20b::AheadLost::Late), std::memory_order_relaxed));
        giveBack(0);
        return false;
    }
    SwitchMetalRangeEncode &encode = gm20bRangeEncode();
    if (const uint32_t veto = cb->_pipelineVeto.load(std::memory_order_acquire)) {
        giveBack(veto);
        return false;
    }
    if (!gRM->raw->channelCurrent(encode.generation())) {
        giveBack(VetoDevice);
        return false;
    }
#if SWITCH_METAL_DIAGNOSTICS
    cb->_pipelinedEncoders = encodersBefore(cb, encode.position());
#endif
    gRM->committed.fetch_sub(1, std::memory_order_release);
    a.cb = nil;
    a.open = false;
    return true;
}

void gm20bPipelineAbandoned(SwitchMetalCommandBuffer *cb, uint32_t veto)
{
    cb->_pipelinedEncoders = 0;
    if (!veto)
        return;
    cb->_pipelineAbandons++;
    cb->_pipelineGivenBack |= veto;
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.pipelineAbandons[__builtin_ctz(veto)]++);
}

void gm20bPipelineOrphans(bool yieldOpen)
{
    Ahead &a = ahead();
    if (!a.cb || a.cb->_committed.load(std::memory_order_acquire))
        return;
    if (CFGetRetainCount((__bridge CFTypeRef)a.cb) == 1)
        giveBack(VetoOtherWork);
    else if (yieldOpen)
        giveBack(VetoDevice);
}

static bool sharesRenderTarget(SwitchMetalCommandBuffer *cb, size_t first, size_t second)
{
    if (cb->_plan.operation(first).op != OpRender || cb->_plan.operation(second).op != OpRender)
        return false;
    const SwitchMetalPass &a = cb->_passes[cb->_plan.legacy(first).x];
    const SwitchMetalPass &b = cb->_passes[cb->_plan.legacy(second).x];
    return a.color[0].texture && a.color[0].texture == b.color[0].texture && b.color[0].load == MTLLoadActionLoad;
}

static void splitCommittedRange(SwitchMetalCommandBuffer *cb)
{
    constexpr size_t kMaxKickRanges = 3, kMinSplitEncoders = 10, kTargetChunkMin = 5;
    if (cb->_segs.size() >= kMaxKickRanges || cb->_predicates.recorded() || !cb->_counterMarks.empty() ||
        !cb->_inlineSignals.empty())
        return;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (!cb->_probeOps.empty() || (cb->_pipelineVeto.load(std::memory_order_acquire) & VetoProbes))
        return;
#endif
    SwitchMetalSegment &range = cb->_segs[cb->_segNext];
    const SwitchMetalCommandPlan &plan = cb->_plan;
    size_t count = 0;
    for (size_t at = range.from; at < range.to;) {
        if (!SwitchMetalCommandPlan::isEncoderBegin(plan.operation(at).op))
            return;
        const size_t end = plan.encoderEnd(at);
        if (end <= at || end > range.to)
            return;
        ++count;
        at = end;
    }
    if (count < kMinSplitEncoders)
        return;
    const size_t room = kMaxKickRanges - cb->_segs.size();
    const size_t parts = std::min(1 + room, std::max<size_t>(2, count / 6));
    const size_t target = std::max(kTargetChunkMin, count / parts);
    const size_t low = kTargetChunkMin / 2, high = count - 2;
    size_t picked = 0, pickedDist = SIZE_MAX, anyCut = 0, anyDist = SIZE_MAX;
    size_t prev = 0, idx = 0;
    for (size_t at = range.from; at < range.to; ++idx) {
        if (idx >= low && idx <= high && !gm20bContinuesPass(cb, at)) {
            const size_t dist = idx >= target ? idx - target : target - idx;
            if (dist <= anyDist) {
                anyCut = at;
                anyDist = dist;
            }
            if (dist <= pickedDist && !sharesRenderTarget(cb, prev, at)) {
                picked = at;
                pickedDist = dist;
            }
        }
        prev = at;
        at = plan.encoderEnd(at);
    }
    const size_t at = picked ? picked : anyCut;
    if (at <= range.from || at >= range.to)
        return;
    SwitchMetalSegment rest = range;
    rest.from = range.to = uint32_t(at);
    rest.gpuWait = false;
    cb->_segs.insert(cb->_segs.begin() + ptrdiff_t(cb->_segNext) + 1, rest);
    SWITCH_METAL_STAT(gm20b::frameCpuCounts().splits.fetch_add(1, std::memory_order_relaxed));
    SWITCH_METAL_STAT(gm20b::frameCpuCounts().rangeSplits.fetch_add(1, std::memory_order_relaxed));
}

static size_t aheadSplitPoint(SwitchMetalCommandBuffer *cb, const SwitchMetalSegment &range)
{
    const Ahead &a = ahead();
    if (a.cb != cb || !a.open || range.from || range.event || range.held || range.from >= range.to)
        return 0;
    const SwitchMetalRangeEncode &encode = gm20bRangeEncode();
    if (cb->_pipelineVeto.load(std::memory_order_acquire) || !gRM->raw->channelCurrent(encode.generation()))
        return 0;
    const size_t stop = stopOf(a);
    const size_t at = stop != SIZE_MAX ? stop : encode.betweenEncoders() ? encode.position() : 0;
    return at > range.from && at < range.to ? at : 0;
}

void gm20bPipelineSplit(SwitchMetalCommandBuffer *cb)
{
    if (cb->_segNext >= cb->_segs.size())
        return;
    SwitchMetalSegment &range = cb->_segs[cb->_segNext];
    if (range.event || range.held || range.from >= range.to)
        return;
    const Ahead &a = ahead();
    if (a.cb == cb && a.open && !range.from) {
        const SwitchMetalRangeEncode &encode = gm20bRangeEncode();
        if (!cb->_pipelineVeto.load(std::memory_order_acquire) && gRM->raw->channelCurrent(encode.generation())) {
            if (const size_t at = aheadSplitPoint(cb, range)) {
                SwitchMetalSegment rest = range;
                rest.from = range.to = uint32_t(at);
                rest.gpuWait = false;
                cb->_segs.insert(cb->_segs.begin() + ptrdiff_t(cb->_segNext) + 1, rest);
                SWITCH_METAL_STAT(gm20b::frameCpuCounts().splits.fetch_add(1, std::memory_order_relaxed));
            }
            return;
        }
    }
    splitCommittedRange(cb);
}

void gm20bPipelineEarlyWork(SwitchMetalCommandBuffer *cb, uint32_t cap, uint32_t &remaining, bool &multiRange)
{
    multiRange = cb->_segs.size() > 1;
    if (!cb->_segs.empty() && aheadSplitPoint(cb, cb->_segs[0]))
        multiRange = true;
    remaining = 0;
    const SwitchMetalCommandPlan &plan = cb->_plan;
    const Ahead &a = ahead();
    const SwitchMetalRangeEncode &encode = gm20bRangeEncode();
    const bool aheadReady = a.cb == cb && a.open && !cb->_pipelineVeto.load(std::memory_order_acquire) &&
                            gRM->raw->channelCurrent(encode.generation()) && stopOf(a) == SIZE_MAX &&
                            encode.betweenEncoders();
    for (size_t s = cb->_segNext; s < cb->_segs.size() && remaining < cap; ++s) {
        const SwitchMetalSegment &seg = cb->_segs[s];
        if (seg.event || seg.held)
            continue;
        size_t at = seg.from;
        if (!s && !seg.from && aheadReady)
            at = std::min<size_t>(encode.position(), seg.to);
        for (; at < seg.to && remaining < cap;) {
            if (!SwitchMetalCommandPlan::isEncoderBegin(plan.operation(at).op)) {
                ++at;
                continue;
            }
            ++remaining;
            const size_t end = plan.encoderEnd(at);
            at = end > at ? end : at + 1;
        }
    }
}

void gm20bPipelineSealed(SwitchMetalCommandBuffer *cb, size_t to)
{
#if SWITCH_METAL_DIAGNOSTICS
    // each raw range of it sealed: the counts are the command buffer's
    const uint32_t encoders = encodersBefore(cb, to);
    if (!cb->_pipelineEncoders)
        gRM->stats.pipelinedEncoders += cb->_pipelinedEncoders;
    gRM->stats.sealedEncoders += encoders - std::min(encoders, cb->_pipelineEncoders);
    cb->_pipelineEncoders = encoders;
#else
    (void)cb;
    (void)to;
#endif
}
