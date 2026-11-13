// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalFresh.h"
#include "RawMetalPlanExecution.h"
#import "RawMetalEventJobs.h"
#include "raw_encode_mallocs.h"
#include "raw_loop_ranges.h"
#include "raw_order.h"
#include "raw_completion_source.h"

// ---- commit: order between the two drivers ----
// With the kernel's Order (TegraGM20BABI.h method 14), the two drivers' command buffers are ordered on the GPU: before a raw
// submission that follows m2v work, one Order(AfterTask) makes its first entry a semaphore acquire on what NVK's channels had
// submitted; before an m2v commit that follows raw work, one Order(BeforeTask) makes NVK's channels' next submissions start with
// an acquire on the raw channel's last seqno. Neither thread waits for the GPU. The one CPU handoff left: a raw command buffer is
// submitted once the m2v ones before it are scheduled (NVK's Exec made, so the AfterTask acquire covers them) - a wait for the
// m2v runtime's own submission of work the GPU is already running, not for that work. What the GPU order cannot cover is
// ordered on the CPU, each with a one-shot line naming it: a kernel without Order, an Order the kernel refuses, a shared event
// signalled outside this device's submitted work.

// What a raw command buffer waits for before it is submitted: GPU order, the m2v command buffers before it scheduled; CPU
// order, completed. Read with no lock: the counts move under m2vLock.
bool m2vClear()
{
    return gRM->gpuOrder ? gRM->m2vUnscheduled == 0 : gRM->m2vPending == 0;
}

// The kernel refused an Order: the drivers are ordered on the CPU until the kernel makes the raw channel again (one line).
void orderRefused(const char *what, int kr)
{
    once([NSString stringWithFormat:@"CPU-ordered: the kernel refused Order (%s, 0x%x)", what, kr]);
    RAW_DIAGNOSTIC(gRM->stats.cpuOrdered++);
    std::lock_guard<raw::Mutex> m(gRM->m2vLock);
    gRM->gpuOrder = false;
    gRM->orderRefusedAt = gRM->raw->stats.reopens.load(std::memory_order_relaxed);
}

// A raw submission went through on a channel the kernel made since it refused an Order: GPU order again
static void orderReturned()
{
    RawState &r = *gRM;
    if (!raw::orderReturns(r.gpuOrder, r.raw->canOrder(), r.raw->stats.reopens.load(std::memory_order_relaxed),
                           r.orderRefusedAt))
        return;
    std::lock_guard<raw::Mutex> m(r.m2vLock);
    r.gpuOrder = true;
}

static void countDriver(int driver)
{
#if SWITCH_RAW_DIAGNOSTICS
    RawState &r = *gRM;
    if (r.lastDriver && r.lastDriver != driver)
        RAW_DIAGNOSTIC(r.stats.switches++);
    r.lastDriver = driver;
#else
    (void)driver;
#endif
}

// Diagnostics: each predicate `cb` hit, once (and once more when it was the only one)
static void countPreds(RMCommandBuffer *cb)
{
#if SWITCH_RAW_DIAGNOSTICS
    const uint64_t m = cb->_preds;
    if (!m)
        return;
    const bool only = !(m & (m - 1));
    for (uint64_t left = m; left; left &= left - 1) {
        const int p = __builtin_ctzll(left);
        gPredCbs[p].fetch_add(1, std::memory_order_relaxed);
        if (only)
            gPredOnly[p].fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)cb;
#endif
}

#if SWITCH_FRAME_TIMELINE
// The m2v replay's parts for the rates line: the wait for an m2v command buffer (its queue's slots), the twins made, and the
// replay's encode end to its commit's return (the Order call, a CPU-order wait, m2v's commit)
static void noteReplay(const switch_frame::Stamps &f, uint64_t returned)
{
    using namespace switch_frame;
    Stats &s = gRM->stats;
    const uint64_t perMs = ticksPerMs();
    auto us = [perMs](uint64_t from, uint64_t to) {
        return to > from ? (to - from) * 1000 / perMs : 0;
    };
    s.replaySlotUs += us(f.get(SlotStart), f.get(SlotEnd));
    s.replayTwinUs += f.get(Twins) * 1000 / perMs;
    s.replayCommitUs += us(f.get(ReplayEnd), returned);
    const uint64_t total = us(f.get(ReplayStart), returned);
    uint64_t seen = s.replayMaxUs.load(std::memory_order_relaxed);
    while (total > seen && !s.replayMaxUs.compare_exchange_weak(seen, total, std::memory_order_relaxed)) {
    }
}
#endif

// Every submission range ends at an encoder boundary. Events are separate operations, so the scheduler can run the
// prefix, publish its signal, and then block its suffix without preventing another queue from producing that suffix's event.
// The command buffer's tape as raw_loop_ranges.h walks it
static auto loopTape(RMCommandBuffer *cb)
{
    return [cb](uint32_t at) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpCompute)
            return raw::LoopTapeEntry{raw::LoopTapeEntry::Compute, uint32_t(c.a)};
        if (c.op == OpRender || c.op == OpBlit || c.op == OpSignalEvent || c.op == OpWaitEvent)
            return raw::LoopTapeEntry{raw::LoopTapeEntry::Other, 0};
        if (c.op == OpPredicate)
            return raw::LoopTapeEntry{raw::LoopTapeEntry::Predicate, uint32_t(c.stage)};
        return raw::LoopTapeEntry{};
    };
}

// `range` as native segments split after each block that holds a loop and no block holds, at `at` in the segment list:
// each one after a segment that lays loops is held (raw_loop_ranges.h). The segments made.
static size_t insertSplitAtLoops(RMCommandBuffer *cb, size_t at, RMSegment range, bool heldFirst)
{
    const auto tape = loopTape(cb);
    std::vector<RMSegment> parts;
    uint32_t from = range.from;
    for (const uint32_t split : raw::loopSplits(tape, range.from, range.to)) {
        RMSegment part = range;
        part.from = from;
        part.to = split;
        parts.push_back(part);
        range.gpuWait = false;
        from = split;
    }
    range.from = from;
    parts.push_back(range);
    for (size_t i = 0; i < parts.size(); ++i)
        parts[i].held = i ? raw::laysLoops(tape, parts[i - 1].from, parts[i - 1].to) : heldFirst;
    cb->_segs.insert(cb->_segs.begin() + ptrdiff_t(at), parts.begin(), parts.end());
    return parts.size();
}

void planSegments(RMCommandBuffer *cb)
{
    cb->_segs.clear();
    cb->_segNext = 0;
    cb->_gpuPrefixEligible = true;
    [cb closeEncoder];
    static thread_local raw::ExecutionPlan plan;  // its capacity kept for the committing thread's next command buffer
    plan.clear();
    // m2v's encoders take no predication records: a command buffer with records the raw path cannot run fails instead
    // of running the work they skip
    cb->_predicates.end();
    if (cb->_predicates.recorded() && (cb->_predicates.misused() || cb->_whole || cb->_reason != WhyRaw)) {
        failCommandBuffer(cb, @"Command predication the raw path cannot run (a misused record, a loop open where its "
                              @"unit ends or refused work)");
        return;
    }
    const auto error = makeExecutionPlan(cb->_plan, cb->_whole, plan, [](const RMPlanEvent &entry) {
        id event = (__bridge id)entry.event;
        return [event respondsToSelector:@selector(signaledValue)] &&
               [event respondsToSelector:@selector(setSignaledValue:)] &&
               [event respondsToSelector:@selector(notifyListener:atValue:block:)];
    });
    if (error != RMPlanError::None) {
        NSString *reason = error == RMPlanError::EventInsideEncoder ? @"An event was encoded inside an active encoder"
                           : error == RMPlanError::NestedEncoder ? @"An encoder began before the previous encoder ended"
                                                                 : @"An encoder did not end before commit";
        failCommandBuffer(cb, reason);
        return;
    }
    for (const auto &range : plan.ranges()) {
        cb->_plan.prepareRange(range.from, range.to);
        const bool native =
            range.kind == raw::ExecutionKind::native && !cb->_plan.nativeRangeRefusal(range.from, range.to);
        cb->_segs.push_back({uint32_t(range.from), uint32_t(range.to), native,
                             range.kind == raw::ExecutionKind::wait || range.kind == raw::ExecutionKind::signal, false,
                             range.gpuWait});
        if (native && cb->_predicates.loops()) {
            const RMSegment whole = cb->_segs.back();
            cb->_segs.pop_back();
            insertSplitAtLoops(cb, cb->_segs.size(), whole, false);
        }
    }
    // A loop's record is read where the range after its range starts: the last range that lays loops gets an empty one
    if (!cb->_segs.empty() && cb->_segs.back().raw && !cb->_segs.back().event &&
        raw::laysLoops(loopTape(cb), cb->_segs.back().from, cb->_segs.back().to)) {
        RMSegment after{cb->_segs.back().to, cb->_segs.back().to, true};
        after.held = true;
        cb->_segs.push_back(after);
    }
    inlineSignals(cb);
}

// The raw path refused `encoder` of the native range being submitted (a draw of it, found while encoding): only that
// encoder goes to m2v, and the encoders around it are submitted raw, each side encoded again as a range of its own. The
// encoder's own range was prepared as it was recorded; nothing here writes the plan, which completion handlers of this
// command buffer's earlier ranges may be reading.
static void isolateRefused(RMCommandBuffer *cb, size_t encoder)
{
    const RMSegment range = cb->_segs[cb->_segNext];
    const size_t end = encoder < range.to ? rmPassGroupEnd(cb, encoder) : range.to;
    std::vector<RMSegment> parts;
    for (const raw::ExecutionRange &part :
         raw::isolate({range.from, range.to, raw::ExecutionKind::native}, encoder, end))
        parts.push_back({uint32_t(part.from), uint32_t(part.to), part.kind == raw::ExecutionKind::native});
    cb->_segs.erase(cb->_segs.begin() + ptrdiff_t(cb->_segNext));
    cb->_segs.insert(cb->_segs.begin() + ptrdiff_t(cb->_segNext), parts.begin(), parts.end());
}

// What the range being submitted knows about its end (raw_completion_source.h)
static raw::RangeEnd rangeEnd(RMCommandBuffer *cb, bool last, bool followed, bool observed)
{
    raw::RangeEnd end;
    end.lastOfBuffer = last;
    if (!last) {
        const RMSegment &next = cb->_segs[cb->_segNext + 1];
        // a range after loops waits for this one's completion (RMSegment::held)
        end.nextRangeRaw = next.raw && !next.event && !next.held;
        end.signals = next.event;
    }
    end.followed = followed;
    end.observed = observed || gRM->completionWaiters.load(std::memory_order_relaxed) != 0;
    end.cpuOrdered = !gRM->gpuOrder;
    return end;
}

// Only the submit worker owns the encoding arenas. A visit emits one legal range and yields its queue's reservation.
// `followed`: another committed command buffer was ready when this one was taken.
bool submitRange(RMCommandBuffer *cb, bool followed)
{
    RawState &r = *gRM;
    NSError *priorError = nil;
    if (cb->_segError.set()) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        priorError = cb->_segError;
    }
    // a present is added before its command buffer is committed, a completed handler's flag before it is added
    const bool observed = cb->_hasCompletedHandlers.load() || cb->_presents;
    if (priorError) {
        finish(cb, priorError);
        return true;
    }
    if (cb->_segNext == cb->_segs.size()) {
        finish(cb, nil);
        return true;
    }
    RMSegment &range = cb->_segs[cb->_segNext];
    const bool last = cb->_segNext + 1 == cb->_segs.size();
    if (range.held) {
        decltype(cb->_loopContinue) loop;
        {
            std::lock_guard<raw::Mutex> l(cb->_lock);
            loop = std::exchange(cb->_loopContinue, {});
        }
        if (loop.pending) {
            const auto pieces = raw::continuation(loopTape(cb), loop.from, loop.to, loop.tag);
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
                const bool held = i && raw::laysLoops(loopTape(cb), pieces[i - 1].from, pieces[i - 1].to);
                at += insertSplitAtLoops(cb, at, RMSegment{pieces[i].from, pieces[i].to, true}, held);
            }
            RAW_DIAGNOSTIC(r.stats.loopContinuations++);
            return false;
        }
        if (range.from == range.to) {
            ++cb->_segNext;
            if (last) {
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
        const RMEventJob job = unsignalled                                ? submitEventJobs(cb)
                               : event.op == OpWaitEvent && range.gpuWait ? submitWaitJob(cb)
                                                                          : RMEventJob::None;
        if (job == RMEventJob::Retry)
            return false;
        if (unsignalled && job == RMEventJob::None) {
            // eventReadiness required all earlier submitted ranges to retire successfully before reaching this position.
            id<MTLSharedEvent> target = (__bridge id)event.obj;
            if (target.signaledValue < event.c)
                target.signaledValue = event.c;
            RAW_DIAGNOSTIC(r.stats.signalsAfterWork++);
        }
        cb->_eventStart = {};
        cb->_eventArmed = 0;
    } else if (range.raw) {
        if (r.gpuOrder && r.m2vSinceRaw) {
            uint32_t waits = 0;
            const int kr = r.raw->orderAfterOthers(waits);
            const raw::OrderAnswer answer = raw::orderAnswer(kr);
            if (answer == raw::OrderAnswer::noChannel) {
                // A channel the next encode made again would run this range unordered after the m2v work before it
                range.raw = false;
                cb->_reason = WhyEncode;
                cb->_why = @"the raw channel is being made again";
                return false;
            }
            if (answer == raw::OrderAnswer::refused) {
                orderRefused("raw after m2v", kr);
                return false;
            }
            r.m2vSinceRaw = false;
            RAW_DIAGNOSTIC(r.stats.acquires += waits);
        }
        const char *why = nullptr;
#if SWITCH_FRAME_TIMELINE
        const uint64_t began = nowUs();
        cb->_frame.mark(switch_frame::EncodeStart);
        raw::EncodeProfile profile;
        std::optional<raw::EncodeMallocs> mallocs(std::in_place, profile);
        const uint64_t profileTicks = raw::encodeTicks();
        raw::tEncodeProfile = &profile;
#endif
        size_t refusedEncoder = range.to;
        cb->_rangeQuiet = raw::quietEnd(rangeEnd(cb, last, followed, observed));
        rmStorageTargetsPlain(cb, range.from, range.to);
        const bool encoded = encodeRaw(cb, range.from, range.to, &why, &refusedEncoder);
#if SWITCH_FRAME_TIMELINE
        const uint64_t wallTicks = raw::encodeTicks() - profileTicks;
        cb->_frame.mark(switch_frame::EncodeEnd);
        RAW_DIAGNOSTIC(r.stats.encodeUs += nowUs() - began);
        const auto folded = [&, wallTicks] {
            raw::tEncodeProfile = nullptr;
            mallocs.reset();
            r.raw->stats.encode.add(profile, wallTicks);
        };
#endif
        if (!encoded) {
#if SWITCH_FRAME_TIMELINE
            folded();
#endif
            {
                std::lock_guard<raw::Mutex> l(cb->_lock);
                priorError = cb->_segError;
            }
            if (priorError) {
                finish(cb, priorError);
                return true;
            }
            if (cb->_resourceReady || waitsOnRetiredProducer(cb))
                return false;
            if (cb->_partial) {
                // raw::CommandBuffer::commit drains its published prefix before reporting a partial failure.
                markWrittenRange(cb, range.from, range.to);
                finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                               code:MTLCommandBufferErrorInternal
                                           userInfo:nil]);
                return true;
            }
            cb->_reason = WhyEncode;
            cb->_why = [NSString stringWithUTF8String:why ?: "?"];
            if (cb->_predicates.recorded()) {
                failCommandBuffer(cb, @"Command predication on work the raw path refused while encoding");
                std::lock_guard<raw::Mutex> l(cb->_lock);
                priorError = cb->_segError;
            }
            if (priorError) {
                finish(cb, priorError);
                return true;
            }
            isolateRefused(cb, refusedEncoder);
            return false;
        }
        RAW_DIAGNOSTIC(r.stats.raw++);
        r.rawSinceM2v = true;
        if (!r.gpuOrder)
            orderReturned();
        countDriver(1);
        {
            RAW_ENCODE_SPAN(Retire);
            retireRawRange(cb, range.from, range.to);
            if (last)
                scheduled(cb);
        }
#if SWITCH_FRAME_TIMELINE
        folded();
#endif
    } else {
        const std::vector<RMSegment> parts = compatibilityParts(cb, range);
        if (parts.size() > 1) {
            cb->_segs.erase(cb->_segs.begin() + ptrdiff_t(cb->_segNext));
            cb->_segs.insert(cb->_segs.begin() + ptrdiff_t(cb->_segNext), parts.begin(), parts.end());
            return false;
        }
        cb->_gpuPrefixEligible = false;
        rmRangePlain(cb, range.from, range.to);
        writeFreshAhead(cb, range.from, range.to);
        if (r.gpuOrder && r.rawSinceM2v) {
            uint32_t waits = 0;
            const int kr = r.raw->orderOthersAfter(waits);
            if (kr != KERN_SUCCESS) {
                orderRefused("m2v after raw", kr);
                return false;
            }
            RAW_DIAGNOSTIC(r.stats.acquires += waits);
        }
#if SWITCH_FRAME_TIMELINE
        const uint64_t began = nowUs();
        cb->_frame.mark(switch_frame::ReplayStart);
        tTwinTicks = 0;
#endif
        id<MTLCommandBuffer> m =
            RM_M2V("a command buffer replayed on m2v") ? replay(cb, range.from, range.to, true) : nil;
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::ReplayEnd);
        cb->_frame.set(switch_frame::Twins, tTwinTicks);
#endif
        if (!m) {
            finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                           code:MTLCommandBufferErrorInternal
                                       userInfo:nil]);
            return true;
        }
        r.rawSinceM2v = false;
        r.m2vSinceRaw = true;
        cb->_m2v = true;
        countDriver(2);
        RAW_DIAGNOSTIC(r.stats.m2v++);
        countPreds(cb);
        const auto writes = captureWrittenRange(cb, range.from, range.to);
        rmRangeHandedOff(cb);
        __block bool counted = false;
        {
            std::lock_guard<raw::Mutex> l(r.m2vLock);
            ++r.m2vPending;
            ++r.m2vUnscheduled;
        }
        [m addScheduledHandler:^(id<MTLCommandBuffer>) {
            // before the raw command buffer behind it may be submitted (m2vClear), so its scheduled handlers go first
            if (last)
                scheduled(cb);
            {
                std::lock_guard<raw::Mutex> l(gRM->m2vLock);
                if (!counted)
                    --gRM->m2vUnscheduled;
                counted = true;
            }
            wakeSubmit(nullptr, RMCompatibilityProgress);
        }];
        [m addCompletedHandler:^(id<MTLCommandBuffer> done) {
            {
                std::lock_guard<raw::Mutex> l(gRM->m2vLock);
                if (!counted)
                    --gRM->m2vUnscheduled;
                counted = true;
                --gRM->m2vPending;
            }
            segmentCompleted(cb, done.error, *writes, RMCompatibilityProgress);
        }];
        cb->_submittedM2v = true;
        [m commit];
#if SWITCH_FRAME_TIMELINE
        RAW_DIAGNOSTIC(r.stats.replayUs += nowUs() - began);
        noteReplay(cb->_frame, cb->_frame.mark(switch_frame::Returned));
#endif
    }
    ++cb->_segNext;
    if (last && range.event)
        scheduled(cb);
    if (last)
        finish(cb, nil);  // joins every submitted range before handlers/resources are released
    return last;
}
