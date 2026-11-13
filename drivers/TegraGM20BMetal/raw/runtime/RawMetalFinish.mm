// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_completion_source.h"
#include "raw_stamps.h"
#if SWITCH_RAW_DIAGNOSTICS
#include "TegraGM20BPathsABI.h"

// The render server's spans of raw_span_marks.h's kLongSpanNs or more in a completed command buffer: logged, and noted to a
// debug kernel for HW Test ui.launch_hitch
static void reportLongSpans(uint32_t stamp, uint64_t gpuNs)
{
    for (const raw::StateSpan &s : gRM->raw->longSpans(stamp)) {
        TegraGM20BAbiPathsSpan note = {TegraGM20BPathsOpNoteSpan, 0, {}};
        raw::formatSpan(note.text, sizeof note.text, s);
        os_log(rawLog(), "raw: long span %{public}s in a %llu us command buffer", note.text,
               (unsigned long long)(gpuNs / 1000));
        gRM->raw->kernelDebugCall(TegraGM20BMethodPaths, &note, sizeof note, nullptr, 0);
    }
}
#endif

static bool markWritten(const raw::RetirementWrites &writes, bool success, const raw::CpuCompletion *cpu = nullptr)
{
    const auto result = success && cpu ? gRM->raw->completeCpu(*cpu) : raw::CpuCompletionResult::Legacy;
    const bool legacy = success && result == raw::CpuCompletionResult::Legacy;
#if defined(__aarch64__)
    bool maintained = false;
#endif
    for (void *object : writes.buffers) {
        RMBuffer *buffer = (__bridge RMBuffer *)object;
        // No-copy clients may keep their original pointer forever. Publish GPU writes before completion or its event,
        // without requiring a new -contents call. The submission flushed dirty CPU lines before any GPU write.
        if (legacy && buffer->_raw.hostVisible && !buffer->_raw.wc && buffer->_raw.cpu) {
#if defined(__aarch64__)
            maintained = true;
            for (uintptr_t at = uintptr_t(buffer->_raw.cpu) & ~uintptr_t(63);
                 at < uintptr_t(buffer->_raw.cpu) + buffer->_raw.length; at += 64)
                __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
#endif
        }
    }
#if defined(__aarch64__)
    if (maintained)
        __asm__ volatile("dsb sy" ::: "memory");
#endif
    for (void *object : writes.buffers) {
        RMBuffer *buffer = (__bridge RMBuffer *)object;
        buffer->_gpuDirty = !buffer->_raw.wc && !(success && (legacy || result == raw::CpuCompletionResult::Current));
    }
    for (void *object : writes.textures)
        ((__bridge RMTexture *)object)->_gpuDirty = true;
    return result != raw::CpuCompletionResult::Failed;
}

void markWrittenRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    markWritten(*captureWrittenRange(cb, from, to), false);
}

// The range's written resources, in a list of the command buffer's own for this range: later kernels can still replace
// plan summaries while its completion runs. Their kinds and CPU buffers are the facts its recording took of them.
const raw::RetirementWrites *captureWrittenRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    if (cb->_retired == cb->_retirements.size())
        cb->_retirements.emplace_back();
    raw::RetirementWrites &writes = cb->_retirements[cb->_retired++];
    const RMCommandPlan &plan = cb->_plan;
    writes.captureInto(
        [&](auto visit) {
            plan.forEachWritten(from, to, visit);
        },
        [&](void *value) {
            if (const RMResourceFacts *facts = plan.factsOf(value))
                return raw::RetirementWrites::Target{facts->cpuBuffer,
                                                     facts->kind == RMResourceKind::Texture ? value : nullptr};
            id object = (__bridge id)value;
            return raw::RetirementWrites::Target{(__bridge void *)cpuBufferOfResource(object),
                                                 [object isKindOfClass:[RMTexture class]] ? value : nullptr};
        });
    return &writes;
}

void rmRangeHandedOff(RMCommandBuffer *cb)
{
    cb->_finishState.submitted();
    gRM->rangesInFlight.fetch_add(1, std::memory_order_relaxed);
}

void retireRawRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    const raw::RetirementWrites *writes = captureWrittenRange(cb, from, to);
    static thread_local std::vector<const raw::Buffer *> cached;
    cached.clear();
    for (void *object : writes->buffers) {
        const raw::Buffer &buffer = ((__bridge RMBuffer *)object)->_raw;
        if (buffer.hostVisible && !buffer.wc && buffer.cpu)
            cached.push_back(&buffer);
    }
    auto cpu = gRM->raw->captureCpuCompletion(cb->_seqno, cb->_channelGeneration, cached);
    rmRangeHandedOff(cb);
    auto signals = publishRawEventSignals(cb);
    RAW_DIAGNOSTIC(gRM->stats.signalsPublished += signals.size());
    const bool quiet = cb->_rangeQuiet;
    // before the hand-off: a completion thread that takes this range reads the promise that came with it
    gRM->submitPromised.store(quiet, std::memory_order_release);
    gRM->inflight.push({cb, cb->_seqno, writes, std::move(signals), cb->_gpuEventWaits, cb->_channelGeneration,
                        std::move(cpu), quiet, std::exchange(cb->_rangeLoopStamp, 0u), uint32_t(from), uint32_t(to)},
                       !quiet);
}

// Its scheduled handlers, taken: run in its queue's order on the queue's handler queue, it scheduled once they ran
static void dispatchScheduled(RMCommandBuffer *cb, NSArray *handlers)
{
    if (!handlers.count) {
        cb->_scheduledDone = true;
        rmChanged(cb);
        return;
    }
    cb->_handlersQueued = true;
    dispatch_async(cb->_queue->_handlers, ^{
        for (MTLCommandBufferHandler h in handlers)
            h((id<MTLCommandBuffer>)cb);
        cb->_scheduledDone = true;
        rmChanged(cb);
    });
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// waitUntilScheduled returned for `cb` while its queue waited on a shared event (`returnedUs`), and it is submitted now, or
// finished without: how far its caller ran ahead of the kernel's writers, at most one line a second, with the returns since
static void noteHeldReturn(RMCommandBuffer *cb, uint64_t returnedUs, const char *how)
{
    static std::atomic<uint64_t> lastUs{0}, since{0};
    since.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = nowUs();
    uint64_t last = lastUs.load(std::memory_order_relaxed);
    if (now - last < 1000000 || !lastUs.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;
    os_log(rawLog(),
           "raw: waitUntilScheduled returned held on a shared event: queue %{public}s, %{public}s %llu us later (%llu "
           "returns since the last line)",
           cb->_queue.label.UTF8String ?: "-", how, (unsigned long long)(now - returnedUs),
           (unsigned long long)since.exchange(0, std::memory_order_relaxed));
}
#endif

// A queue's command buffers' scheduled handlers run in its order, on its own serial queue: one that waits for another queue's
// command buffer holds up none of that queue's handlers.
void scheduled(RMCommandBuffer *cb)
{
    unhold(cb);
    if (cb->_scheduledSent.exchange(true))
        return;  // once (eventHeld: at its commit; then its submission's)
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t heldReturnUs = cb->_heldReturnUs.exchange(0);
#endif
    MTLCommandBufferStatus committed = MTLCommandBufferStatusCommitted;
    cb->_status.compare_exchange_strong(committed, MTLCommandBufferStatusScheduled);
    const double now = hostSeconds();
    cb->_kernelEnd = now;
    cb->_gpuStart = now;
    // Core Animation's render thread waits for this every frame (waitUntilScheduled): with nothing to run, no hop through
    // the handler queue
    dispatchScheduled(cb, rmTakeHandlers(cb, cb->_scheduledHandlers, cb->_hasScheduledHandlers));
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (heldReturnUs)
        noteHeldReturn(cb, heldReturnUs, "submitted");
#endif
}

// Completed: CPU-signalled events, written resources marked, handlers (on the queue's completion queue when it has one), the
// objects released, waiters woken.
// The raw path's empty chunks go back as m2v's blocks do: on memory pressure, and outside the render server once nothing completed
// for NMB_IDLE_RELEASE_MS (each completion moves the deadline)
GpuMemoryRelease &gRawRelease = *new GpuMemoryRelease;

static void finished(RMCommandBuffer *cb);

void segmentCompleted(RMCommandBuffer *cb, NSError *error, const raw::RetirementWrites &writes, uint32_t dependencies,
                      const std::vector<RMNativeEventSignal> &signals,
                      const std::vector<raw::EventTimeline::Promise> &waits, const raw::CpuCompletion *cpu)
{
    if (error && ![error.domain isEqualToString:MTLCommandBufferErrorDomain])
        error = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                    code:MTLCommandBufferErrorInternal
                                userInfo:@{
                                    NSLocalizedDescriptionKey : error.localizedDescription ?: @"",
                                    NSUnderlyingErrorKey : error
                                }];
    if (!error && raw::eventDependencies(waits, 0, false) != raw::EventDependencyState::ready)
        error = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:nil];
    if (!markWritten(writes, !error, cpu) && !error)
        error =
            [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                code:MTLCommandBufferErrorInternal
                            userInfo:@{NSLocalizedDescriptionKey : @"CPU-visible GPU results could not be acquired"}];
    retireEventSignals(signals, !error);
    if (error) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        if (!cb->_segError)
            cb->_segError = error;
    }
    const bool last = cb->_finishState.completed();
    if (gRM->rangesInFlight.fetch_sub(1, std::memory_order_acq_rel) == 1)
        gRawRelease.drained();
    if (gRM->progress.progressed())  // a queue head is parked: it is judged again (raw_progress_gate.h)
        wakeSubmit((__bridge void *)cb->_queue, dependencies);
    if (last)
        finished(cb);
}

// A command buffer's completed handlers (`run`): on its queue's completion queue, else on the following threads, behind its
// scheduled handlers where those went to its queue's handler queue; inline on this thread where it has neither handlers nor
// a completion queue. Never on the handler queue itself: a slow completed handler of one frame would hold up the next
// frame's presentDrawable. The queue's turns keep its completed handlers one at a time, in order.
static void deliverCompletion(RMCommandBuffer *cb);

// Its completed handlers run, it is done, and the command buffer behind it that waited for its turn is delivered
static void completeBuffer(RMCommandBuffer *cb)
{
    NSArray *handlers = cb->_doneHandlers;
    cb->_doneHandlers = nil;
    for (MTLCommandBufferHandler h in handlers)
        h((id<MTLCommandBuffer>)cb);
    cb->_scheduledDone = true;
    cb->_completedDone = true;
    rmChanged(cb);
    cb->_owners.clear();  // the resources may die now: no submission uses them any more (its capacity is recycled)
    rmBytesDone(cb);
    cb->_computeJobs.clear();
    cb->_sharedEventWaits.clear();
    cb->_resourceReady.reset();
    gRM->committed.fetch_sub(1, std::memory_order_acq_rel);
#if SWITCH_FRAME_TIMELINE
    switch_frame::finished(cb->_frame);
#endif
    void *last = (__bridge void *)cb;
    if (cb->_queue->_last.compare_exchange_strong(last, nullptr))
        CFRelease(last);
    if (void *next = cb->_turn.finished())
        deliverCompletion((__bridge_transfer RMCommandBuffer *)next);
}

// dispatch_async_f's work: the command buffer the context holds
static void completeHeld(void *context)
{
    completeBuffer((__bridge_transfer RMCommandBuffer *)context);
}

static void completeOnTarget(void *context)
{
    dispatch_queue_t q = ((__bridge RMCommandBuffer *)context)->_queue->_completionQueue;
    dispatch_async_f(q ?: followingRootQueue(), context, completeHeld);
}

// Inline where it has neither handlers nor a completion queue nor scheduled handlers queued ahead of them
static void deliverCompletion(RMCommandBuffer *cb)
{
    dispatch_queue_t q = cb->_queue->_completionQueue;
    if (!cb->_doneBehindScheduled && !q && !cb->_doneHandlers.count) {
        completeBuffer(cb);
        return;
    }
    void *context = (__bridge_retained void *)cb;
    if (cb->_doneBehindScheduled)
        dispatch_async_f(cb->_queue->_handlers, context, completeOnTarget);
    else
        dispatch_async_f(q ?: followingRootQueue(), context, completeHeld);
}

void failCommandBuffer(RMCommandBuffer *cb, NSString *reason, MTLCommandBufferError code)
{
    NSError *error = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                         code:code
                                     userInfo:@{NSLocalizedDescriptionKey : reason}];
    std::lock_guard<raw::Mutex> l(cb->_lock);
    if (!cb->_segError)
        cb->_segError = error;
}

void finish(RMCommandBuffer *cb, NSError *error)
{
    if (error) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        if (!cb->_segError)
            cb->_segError = error;
    }
    if (cb->_finishState.ready()) {
        finished(cb);
        return;
    }
    // A failed command buffer submits no more: its waitUntilScheduled returns now, not once its earlier work completes
    if (error)
        rmChanged(cb);
}

// It submits no more and nothing of it is in flight: completed, once (raw_finish_state.h)
static void finished(RMCommandBuffer *cb)
{
    NSError *error = nil;
    if (cb->_segError.set()) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        error = cb->_segError;
    }
    RAW_DIAGNOSTIC(gRM->stats.finished.fetch_add(1, std::memory_order_relaxed));
    cancelPromises(cb);
    releaseEventWaits(cb);
    unhold(cb);
    // A completed command buffer is scheduled. Its scheduled handlers scheduled() never took run here, before the completed ones:
    // one whose fence passed before its submitting thread's scheduled() (the completion thread wakes on work it was just handed),
    // one that never reached a GPU. Ones scheduled() already queued run first, the completed ones queued behind them on the
    // handler queue.
    const bool sendScheduled = !cb->_scheduledSent.exchange(true);
    NSArray *scheduledHandlers = nil;
    if (sendScheduled) {
        scheduledHandlers = rmTakeHandlers(cb, cb->_scheduledHandlers, cb->_hasScheduledHandlers);
        cb->_kernelEnd = hostSeconds();
#if SWITCH_FRAME_TIMELINE
        RAW_DIAGNOSTIC(gRM->stats.scheduledByFinish++);
#endif
    }
    {
        cb->_error = error;
        cb->_status = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
        // the GPU's own time, its end where this thread saw it complete: the duration exact, both late by that latency
        uint64_t ns = 0;
        const bool stamped = !error && !cb->_m2v && gRM->raw->gpuNanoseconds(cb->_stamp, ns);
        gRM->raw->learnCompleted();
        RAW_DIAGNOSTIC(cb->_gpuStamped = stamped);
        const raw::CommandBufferTimes t =
            raw::orderedTimes(cb->_kernelStart, cb->_kernelEnd, hostSeconds(), stamped, ns);
        cb->_kernelStart = t.kernelStart;
        cb->_kernelEnd = t.kernelEnd;
        cb->_gpuStart = t.gpuStart;
        cb->_gpuEnd = t.gpuEnd;
    }
    cb->_completedSent = true;
    NSArray *handlers = rmTakeHandlers(cb, cb->_completedHandlers, cb->_hasCompletedHandlers);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t heldReturnUs = cb->_heldReturnUs.exchange(0);
#endif
    if (sendScheduled)
        dispatchScheduled(cb, scheduledHandlers);
    const bool behindScheduled = cb->_handlersQueued;
    rmChanged(cb);
#if SWITCH_RAW_DIAGNOSTICS
    if (cb->_gpuStamped)
        reportLongSpans(cb->_stamp, uint64_t((cb->_gpuEnd - cb->_gpuStart) * 1e9));
#endif
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (heldReturnUs)
        noteHeldReturn(cb, heldReturnUs, error ? "failed unsubmitted" : "finished unsubmitted");
#endif
    // A handler may submit and wait for another queue's command buffer, so none runs on a GPU worker. One that waits for a
    // later command buffer of its own queue waits for good, as it does on Metal's serial per-queue completion queue.
    cb->_doneHandlers = handlers;
    cb->_doneBehindScheduled = behindScheduled;
    RMCommandBuffer *before = cb->_before;
    cb->_before = nil;
    if (before && !before->_turn.ran()) {
        void *later = (__bridge_retained void *)cb;
        if (before->_turn.hand(later))
            return;
        (void)(__bridge_transfer RMCommandBuffer *)later;
    }
    deliverCompletion(cb);
}

// A command buffer the kernel killed the channel of: as Metal reports the GPU's own failures (TegraGM20BAbiFence error:
// 1 MMU fault, 4 timeout)
static NSError *killError(uint32_t kernelError)
{
    const MTLCommandBufferError code = kernelError == 1   ? MTLCommandBufferErrorPageFault
                                       : kernelError == 4 ? MTLCommandBufferErrorTimeout
                                                          : MTLCommandBufferErrorInternal;
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:code userInfo:nil];
}

// The completion thread: one source per device (raw_completion_source.h). It sleeps in the kernel on the first raw range
// in flight whose job interrupts, then retires each range up to it in order, those finding their fence already passed.
void completionLoop()
{
    RawState &r = *gRM;
    for (;;) {
        @autoreleasepool {
            std::optional<size_t> target;
            r.inflight.waitFor([&](const auto &taken) {
                target = raw::completionTarget(taken, r.submitPromised.load(std::memory_order_acquire),
                                               r.completionWaiters.load(std::memory_order_acquire) != 0);
                return target.has_value();
            });
            if (*target) {
                const RMRawRetirement &cover = r.inflight.at(*target);
                (void)r.raw->wait(cover.seqno, cover.generation);
            }
            RMRawRetirement retired = std::move(r.inflight.front());
            RMCommandBuffer *cb = retired.buffer;
            // false: the kernel killed the channel with it in flight (a GPU fault or hang); the next command buffer gets a new one
            const bool ok = r.raw->wait(retired.seqno, retired.generation);
            if (!ok && r.raw->lostError() == raw::Device::kWaitRefused) {
                os_log_error(
                    rawLog(),
                    "raw: command buffer %u failed: the kernel kept refusing to wait for it on a channel it called "
                    "healthy, so the channel was taken as killed",
                    retired.seqno);
            } else if (!ok && r.raw->lostError() == raw::Device::kDependencyFailed) {
                os_log_error(rawLog(), "raw: command buffer %u failed an upstream dependency", retired.seqno);
            } else if (!ok) {
                const uint64_t va = r.raw->lostFaultVa();
                os_log_error(
                    rawLog(),
                    "raw: command buffer %u failed: the kernel killed its channel (error %u, fault VA 0x%llx%s%s)",
                    retired.seqno, r.raw->lostError(), (unsigned long long)va, va ? ": " : "",
                    va ? r.raw->vaOwner(va).c_str() : "");
            }
#if SWITCH_FRAME_TIMELINE
            cb->_frame.mark(switch_frame::Fence);
            switch_frame::trace(switch_frame::kFence, switch_frame::kPoint, cb->_frame.seq);
#endif
            r.inflight.pop();
            NSError *error = ok ? nil : killError(r.raw->lostError());
            if (const raw::LoopContinue loop = r.raw->loopContinue(retired.loopStamp); !error && loop.continues) {
                std::lock_guard<raw::Mutex> l(cb->_lock);
                cb->_loopContinue = {true, loop.tag, retired.loopFrom, retired.loopTo};
            }
            segmentCompleted(cb, error, *retired.writes, RMRawProgress, retired.signals, retired.waits, &retired.cpu);
        }
    }
}
