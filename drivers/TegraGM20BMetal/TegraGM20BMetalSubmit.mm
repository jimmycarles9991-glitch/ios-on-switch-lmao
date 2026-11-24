// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "TegraGM20BMetalPlanExecution.h"
#import "TegraGM20BMetalEventJobs.h"
#include "encode_mallocs.h"
#include "loop_ranges.h"
#include "completion_source.h"
#import "TegraGM20BMetalPipeline.h"

// `cb` failed for `reason`, finished: what a submission that cannot go on returns
static bool failSubmission(SwitchMetalCommandBuffer *cb, NSString *reason)
{
    failCommandBuffer(cb, reason);
    NSError *error = nil;
    {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        error = cb->_segError;
    }
    finish(cb, error);
    return true;
}

// Every submission range ends at an encoder boundary. Events are separate operations, so the scheduler can run the
// prefix, publish its signal, and then block its suffix without preventing another queue from producing that suffix's event.
// The command buffer's tape as loop_ranges.h walks it
static auto loopTape(SwitchMetalCommandBuffer *cb)
{
    return [cb](uint32_t at) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpCompute)
            return gm20b::LoopTapeEntry{gm20b::LoopTapeEntry::Compute, uint32_t(c.a)};
        if (c.op == OpRender || c.op == OpBlit || c.op == OpSignalEvent || c.op == OpWaitEvent)
            return gm20b::LoopTapeEntry{gm20b::LoopTapeEntry::Other, 0};
        if (c.op == OpPredicate)
            return gm20b::LoopTapeEntry{gm20b::LoopTapeEntry::Predicate, uint32_t(c.stage)};
        return gm20b::LoopTapeEntry{};
    };
}

// `range` as native segments split after each block that holds a loop and no block holds, at `at` in the segment list:
// each one after a segment that lays loops is held (loop_ranges.h). The segments made.
static size_t insertSplitAtLoops(SwitchMetalCommandBuffer *cb, size_t at, SwitchMetalSegment range, bool heldFirst)
{
    const auto tape = loopTape(cb);
    std::vector<SwitchMetalSegment> parts;
    uint32_t from = range.from;
    for (const uint32_t split : gm20b::loopSplits(tape, range.from, range.to)) {
        SwitchMetalSegment part = range;
        part.from = from;
        part.to = split;
        parts.push_back(part);
        range.gpuWait = false;
        from = split;
    }
    range.from = from;
    parts.push_back(range);
    for (size_t i = 0; i < parts.size(); ++i)
        parts[i].held = i ? gm20b::laysLoops(tape, parts[i - 1].from, parts[i - 1].to) : heldFirst;
    cb->_segs.insert(cb->_segs.begin() + ptrdiff_t(at), parts.begin(), parts.end());
    return parts.size();
}

static NSString *planErrorText(SwitchMetalPlanError error)
{
    switch (error) {
    case SwitchMetalPlanError::EventInsideEncoder:
        return @"An event was encoded inside an active encoder";
    case SwitchMetalPlanError::NestedEncoder:
        return @"An encoder began before the previous encoder ended";
    case SwitchMetalPlanError::CommandAfterEnd:
        return @"A command was encoded on an encoder that had ended";
    case SwitchMetalPlanError::CommandAfterOtherWork:
        return @"A command was encoded on an encoder that had ended, after an encoder of another kind or an event "
               @"began";
    default:
        return @"An encoder did not end before commit";
    }
}

// The commands recorded on an encoder after it ended, before anything else began, moved inside it before its OpEnd,
// where an M4 Max with validation off runs them; then the tape planned again. Commands after an event, or of another
// kind than the encoder whose end they follow, are the command buffer's error (InvalidResource). The submit worker
// encodes ahead only the tape before the position `_published` names and reads the encoder that begins there
// (TegraGM20BMetalPipeline.h): an encoder past that position is the recording's alone and its end moves with nothing taken
// back from the worker; at or before it, the run fails the command buffer.
static SwitchMetalPlanError joinEndedEncoders(SwitchMetalCommandBuffer *cb, gm20b::ExecutionPlan &plan)
{
    SwitchMetalCommandPlan &commands = cb->_plan;
    const size_t ahead = cb->_handedOnce ? size_t(cb->_published.load(std::memory_order_relaxed) >> 32) : 0;
    SwitchMetalPlanError error = SwitchMetalPlanError::None;
    const char *blocked = nullptr;
    forEachLateRun(commands, [&](size_t encoder, size_t from, size_t to) {
        for (size_t at = from; at < to; ++at) {
            const Op op = commands.operation(at).op;
            if (encoder == SIZE_MAX || !recordedBy(commands.operation(encoder).op, op)) {
                error = SwitchMetalPlanError::CommandAfterOtherWork;
                return false;
            }
        }
        if (ahead && ahead >= encoder)
            blocked = "the encode ahead of its commit may have read that encoder's end";
        if (blocked) {
            error = SwitchMetalPlanError::CommandAfterEnd;
            return false;
        }
        const size_t end = commands.encoderEnd(encoder);
        commands.joinEnded(encoder, to);
        for (SwitchMetalCounterMark &mark : cb->_counterMarks)
            if (mark.at == end - 1)
                mark.at = uint32_t(to - 1);
        return true;
    });
    if (blocked)
        SWITCH_REFUSE("gm20b", "plan.ended_encoder",
                      "a command encoded on an encoder that had ended, not moved inside it (%s): its command buffer "
                      "fails MTLCommandBufferErrorInternal",
                      blocked);
    if (error != SwitchMetalPlanError::None)
        return error;
    plan.clear();
    return makeExecutionPlan(commands, plan);
}

void planSegments(SwitchMetalCommandBuffer *cb)
{
    cb->_segs.clear();
    cb->_segNext = 0;
    cb->_gpuPrefixEligible = true;
    [cb closeEncoder];
    static thread_local gm20b::ExecutionPlan plan;  // its capacity kept for the committing thread's next command buffer
    plan.clear();
    // a command buffer with records the driver cannot run fails instead of running the work they skip
    cb->_predicates.end();
    if (cb->_predicates.recorded() && cb->_predicates.misused()) {
        failCommandBuffer(cb, @"Command predication the driver cannot run (a misused record or a loop open where its "
                              @"unit ends)");
        return;
    }
    auto error = makeExecutionPlan(cb->_plan, plan);
    if (error != SwitchMetalPlanError::None) {
        if (error == SwitchMetalPlanError::CommandAfterEnd)
            error = joinEndedEncoders(cb, plan);
        if (error == SwitchMetalPlanError::CommandAfterOtherWork)
            SWITCH_REFUSE("gm20b", "plan.ended_kind",
                          "a command encoded on an encoder that had ended, after an encoder of another kind or an "
                          "event began: Metal's validation error, which an M4 Max crashes on; its command buffer "
                          "fails MTLCommandBufferErrorInvalidResource");
        if (error != SwitchMetalPlanError::None) {
            failCommandBuffer(cb, planErrorText(error),
                              error == SwitchMetalPlanError::CommandAfterOtherWork
                                  ? MTLCommandBufferErrorInvalidResource
                                  : MTLCommandBufferErrorInternal);
            return;
        }
    }
    // with room for the split at the encode ahead's end (gm20bPipelineSplit), which the worker makes without allocating
    cb->_segs.reserve(plan.ranges().size() + 2);
    // the ranges are prepared by the submit worker at their turn (submitLoop), which owns the plan from the commit on
    for (const auto &range : plan.ranges()) {
        const bool native = range.kind == gm20b::ExecutionKind::native;
        cb->_segs.push_back({uint32_t(range.from), uint32_t(range.to), !native, false, range.gpuWait});
        if (native && cb->_predicates.loops()) {
            const SwitchMetalSegment whole = cb->_segs.back();
            cb->_segs.pop_back();
            insertSplitAtLoops(cb, cb->_segs.size(), whole, false);
        }
    }
    // A loop's record is read where the range after its range starts: the last range that lays loops gets an empty one
    if (!cb->_segs.empty() && !cb->_segs.back().event &&
        gm20b::laysLoops(loopTape(cb), cb->_segs.back().from, cb->_segs.back().to)) {
        SwitchMetalSegment after{cb->_segs.back().to, cb->_segs.back().to};
        after.held = true;
        cb->_segs.push_back(after);
    }
    inlineSignals(cb);
}

// An encode that stopped with no error of its command buffer: every refusal of the range encoder fails it, so this is a
// refusal that did not, named by its why
static NSString *stoppedWithoutError(const char *why)
{
    const char *named = why ?: "no reason given";
    os_log_error(gm20bLog(),
                 "SwitchMetal: a range's encode stopped with no command buffer error (%{public}s): it fails", named);
    return [NSString stringWithFormat:@"The GPU driver stopped encoding the command buffer: %s", named];
}

// What the range being submitted knows about its end (completion_source.h)
static gm20b::RangeEnd rangeEnd(SwitchMetalCommandBuffer *cb, bool last, bool followed, bool handlers)
{
    gm20b::RangeEnd end;
    end.lastOfBuffer = last;
    if (!last) {
        const SwitchMetalSegment &next = cb->_segs[cb->_segNext + 1];
        // a range after loops waits for this one's completion (SwitchMetalSegment::held)
        end.nextRangeRaw = !next.event && !next.held;
        end.signals = next.event;
    }
    end.followed = followed;
    const bool waited = cb->_presents || gRM->completionWaiters.load(std::memory_order_relaxed) != 0;
    end.observed = handlers || waited;
    end.handlersOnly = handlers && !waited;
    end.compositor = gRM->raw->role() == gm20b::Device::Role::RenderServer;
    return end;
}

// Only the submit worker owns the encoding arenas. A visit emits one legal range and yields its queue's reservation.
// `followed`: another committed command buffer was ready when this one was taken.
bool submitRange(SwitchMetalCommandBuffer *cb, bool followed)
{
    [[maybe_unused]] GM20BState &r = *gRM;
    NSError *priorError = nil;
    if (cb->_segError.set()) {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        priorError = cb->_segError;
    }
    // a present is added before its command buffer is committed, a completed handler's flag before it is added
    const bool handlers = cb->_hasCompletedHandlers.load();
    {
        const bool more = !priorError && cb->_segNext < cb->_segs.size();
        const SwitchMetalSegment *next = more ? &cb->_segs[cb->_segNext] : nullptr;
        gm20bPipelineYield(cb, next ? next->from : SIZE_MAX, next && !next->event && !next->held);
    }
    if (priorError) {
        finish(cb, priorError);
        return true;
    }
    if (cb->_segNext == cb->_segs.size()) {
        cb->_submittedAll.store(true, std::memory_order_release);
        finish(cb, nil);
        return true;
    }
    const SwitchMetalSegment &range = cb->_segs[cb->_segNext];
    const bool last = cb->_segNext + 1 == cb->_segs.size();
    if (range.held) {
        decltype(cb->_loopContinue) loop;
        {
            std::lock_guard<gm20b::Mutex> l(cb->_lock);
            loop = std::exchange(cb->_loopContinue, {});
        }
        if (loop.pending) {
            const auto pieces = gm20b::continuation(loopTape(cb), loop.from, loop.to, loop.tag);
            if (pieces.empty()) {
                finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                               code:MTLCommandBufferErrorInternal
                                           userInfo:@{
                                               NSLocalizedDescriptionKey :
                                                   @"A predicated loop's record names no loop of the range that laid it"
                                           }]);
                return true;
            }
            size_t at = cb->_segNext;
            for (size_t i = 0; i < pieces.size(); ++i) {
                const bool held = i && gm20b::laysLoops(loopTape(cb), pieces[i - 1].from, pieces[i - 1].to);
                at += insertSplitAtLoops(cb, at, SwitchMetalSegment{pieces[i].from, pieces[i].to}, held);
            }
            SWITCH_METAL_DIAGNOSTIC(r.stats.loopContinuations++);
            return false;
        }
        if (range.from == range.to) {
            ++cb->_segNext;
            if (last) {
                cb->_submittedAll.store(true, std::memory_order_release);
                scheduled(cb);
                finish(cb, nil);
            }
            return last;
        }
    }
    if (range.event) {
        const Cmd event = cb->_plan.legacy(range.from);
        if (event.op == OpWaitEvent)
            releaseEventWaits(cb);
        const bool unsignalled = event.op == OpSignalEvent && !range.gpuSignal;
        const SwitchMetalEventJob job = unsignalled                                ? submitEventJobs(cb)
                                        : event.op == OpWaitEvent && range.gpuWait ? submitWaitJob(cb)
                                                                                   : SwitchMetalEventJob::None;
        if (job == SwitchMetalEventJob::Retry)
            return false;
        if (unsignalled && job == SwitchMetalEventJob::None) {
            // eventReadiness required all earlier submitted ranges to retire successfully before reaching this position.
            id<MTLSharedEvent> target = (__bridge id)event.obj;
            if (target.signaledValue < event.c)
                target.signaledValue = event.c;
            SWITCH_METAL_DIAGNOSTIC(r.stats.signalsAfterWork++);
        }
        cb->_eventStart = {};
        cb->_eventArmed = 0;
    } else {
        const char *why = nullptr;
#if SWITCH_FRAME_TIMELINE
        const uint64_t began = nowUs();
        cb->_frame.mark(switch_frame::EncodeStart);
        gm20b::EncodeProfile profile;
        std::optional<gm20b::EncodeMallocs> mallocs(std::in_place, profile);
        const uint64_t profileTicks = gm20b::encodeTicks();
        gm20b::tEncodeProfile = &profile;
#endif
        const gm20b::RangeEnd end = rangeEnd(cb, last, followed, handlers);
        cb->_rangeQuiet = gm20b::quietEnd(end);
        cb->_rangeHeld = gm20b::frameHeld(end);
        SwitchMetalEncoded encoded = SwitchMetalEncoded::stopped;
        @autoreleasepool {
            gm20bMoveWanting();
            encoded = encodeRaw(cb, range.from, range.to, &why);
        }
#if SWITCH_FRAME_TIMELINE
        const uint64_t wallTicks = gm20b::encodeTicks() - profileTicks;
        cb->_frame.mark(switch_frame::EncodeEnd);
        SWITCH_METAL_DIAGNOSTIC(r.stats.encodeUs += nowUs() - began);
        SWITCH_METAL_DIAGNOSTIC(cb->_scheduleNotes.encodeUs.fetch_add(nowUs() - began, std::memory_order_relaxed));
        const auto folded = [&, wallTicks] {
            gm20b::tEncodeProfile = nullptr;
            mallocs.reset();
            r.raw->stats.encode.add(profile, wallTicks);
        };
#endif
        if (encoded != SwitchMetalEncoded::submitted) {
#if SWITCH_FRAME_TIMELINE
            folded();
#endif
            {
                std::lock_guard<gm20b::Mutex> l(cb->_lock);
                priorError = cb->_segError;
            }
            if (priorError) {
                finish(cb, priorError);
                return true;
            }
            // Visited again: eventReadiness parks the queue until the producer's outcome, or the resource readiness the
            // seal armed, wakes it
            if (cb->_resourceReady || encoded == SwitchMetalEncoded::producerPending)
                return false;
            return failSubmission(cb, stoppedWithoutError(why));
        }
        SWITCH_METAL_DIAGNOSTIC(r.stats.raw++);
#if SWITCH_METAL_DIAGNOSTICS
        if (!cb->_kickedTicks)
            cb->_kickedTicks = mach_absolute_time();
#endif
        {
            SWITCH_METAL_ENCODE_SPAN(Retire);
            // Core Animation's waitUntilScheduled returns before the retirement's bookkeeping: a waitUntilCompleted that
            // follows finds its range in flight or wakes the completion thread for it (retireRawRange)
            if (last) {
                cb->_submittedAll.store(true, std::memory_order_release);
                scheduled(cb);
            }
            retireRawRange(cb, range.from, range.to);
        }
#if SWITCH_FRAME_TIMELINE
        folded();
#endif
    }
    ++cb->_segNext;
    if (last && range.event) {
        cb->_submittedAll.store(true, std::memory_order_release);
        scheduled(cb);
    }
    if (last)
        finish(cb, nil);  // joins every submitted range before handlers/resources are released
    return last;
}
