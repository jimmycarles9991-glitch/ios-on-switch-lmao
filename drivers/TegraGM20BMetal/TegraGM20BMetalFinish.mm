// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalPipeline.h"
#include <time.h>
#include "completion_source.h"
#include "stamps.h"
#if SWITCH_METAL_PROF
#include "TegraGM20BPathsABI.h"

// The render server's spans of span_marks.h's kLongSpanNs or more in a completed command buffer: logged, and noted to a
// debug kernel for HW Test ui.launch_hitch
static void reportLongSpans(uint32_t stamp, uint64_t gpuNs)
{
    for (const gm20b::StateSpan &s : gRM->raw->longSpans(stamp)) {
        TegraGM20BAbiPathsSpan note = {TegraGM20BPathsOpNoteSpan, 0, {}};
        char text[192];
        gm20b::formatSpan(text, sizeof text, s);
        strlcpy(note.text, text, sizeof note.text);
        os_log(gm20bLog(), "SwitchMetal: long span %{public}s in a %llu us command buffer", text,
               (unsigned long long)(gpuNs / 1000));
        gRM->raw->kernelDebugCall(TegraGM20BMethodPaths, &note, sizeof note, nullptr, 0);
    }
}
#endif

static bool markWritten(const gm20b::RetirementWrites &writes, bool success, const gm20b::CpuCompletion *cpu = nullptr)
{
    const auto result = success && cpu ? gRM->raw->completeCpu(*cpu) : gm20b::CpuCompletionResult::Legacy;
    const bool legacy = success && result == gm20b::CpuCompletionResult::Legacy;
#if defined(__aarch64__)
    bool maintained = false;
#endif
    for (void *object : writes.buffers) {
        SwitchMetalBuffer *buffer = (__bridge SwitchMetalBuffer *)object;
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
        SwitchMetalBuffer *buffer = (__bridge SwitchMetalBuffer *)object;
        buffer->_gpuDirty = !buffer->_raw.wc && !(success && (legacy || result == gm20b::CpuCompletionResult::Current));
    }
    for (void *object : writes.textures)
        ((__bridge SwitchMetalTexture *)object)->_gpuDirty = true;
    return result != gm20b::CpuCompletionResult::Failed;
}

void markWrittenRange(SwitchMetalCommandBuffer *cb, size_t from, size_t to)
{
    markWritten(*captureWrittenRange(cb, from, to), false);
}

// The range's written resources, in a list of the command buffer's own for this range: later kernels can still replace
// plan summaries while its completion runs. Their kinds and CPU buffers are the facts its recording took of them.
const gm20b::RetirementWrites *captureWrittenRange(SwitchMetalCommandBuffer *cb, size_t from, size_t to)
{
    if (cb->_retired == cb->_retirements.size())
        cb->_retirements.emplace_back();
    gm20b::RetirementWrites &writes = cb->_retirements[cb->_retired++];
    const SwitchMetalCommandPlan &plan = cb->_plan;
    writes.captureInto(
        [&](auto visit) {
            plan.forEachWritten(from, to, visit);
            for (const auto &snapshot : cb->_residencySnapshots)
                for (const SwitchMetalResidencyEntry &entry : snapshot->hostVisible) {
                    if (entry.kind == SwitchMetalResidencyEntry::Kind::Buffer) {
                        SwitchMetalBuffer *b = (SwitchMetalBuffer *)entry.allocation;
                        if (b->_raw.hostVisible && !b->_raw.wc)
                            visit((__bridge void *)entry.allocation);
                    } else if (entry.kind == SwitchMetalResidencyEntry::Kind::Texture && entry.write) {
                        SwitchMetalTexture *t = (SwitchMetalTexture *)entry.allocation;
                        if (t->_idNamed.load(std::memory_order_acquire) ||
                            t->_viewIdNamed.load(std::memory_order_acquire))
                            visit((__bridge void *)entry.allocation);
                    }
                }
        },
        [&](void *value) {
            if (const SwitchMetalResourceFacts *facts = plan.factsOf(value))
                return gm20b::RetirementWrites::Target{
                    facts->cpuBuffer, facts->kind == SwitchMetalResourceKind::Texture ? value : nullptr};
            id object = (__bridge id)value;
            return gm20b::RetirementWrites::Target{(__bridge void *)cpuBufferOfResource(object),
                                                   [object isKindOfClass:[SwitchMetalTexture class]] ? value : nullptr};
        });
    return &writes;
}

void gm20bRangeHandedOff(SwitchMetalCommandBuffer *cb)
{
    cb->_finishState.submitted();
}

void retireRawRange(SwitchMetalCommandBuffer *cb, size_t from, size_t to)
{
    const gm20b::RetirementWrites *writes = captureWrittenRange(cb, from, to);
    static thread_local std::vector<const gm20b::Buffer *> cached;
    cached.clear();
    for (void *object : writes->buffers) {
        const gm20b::Buffer &buffer = ((__bridge SwitchMetalBuffer *)object)->_raw;
        if (buffer.hostVisible && !buffer.wc && buffer.cpu)
            cached.push_back(&buffer);
    }
    auto cpu = gRM->raw->captureCpuCompletion(cb->_seqno, cb->_channelGeneration, cached);
    gm20bRangeHandedOff(cb);
    auto signals = publishRawEventSignals(cb);
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.signalsPublished += signals.size());
    const bool quiet = cb->_rangeQuiet;
    const uint64_t frameDue =
        quiet && cb->_rangeHeld ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + gm20b::kFrameEndNs : 0;
    // before the hand-off: a completion thread that takes this range reads the promise that came with it
    gRM->submitPromised.store(quiet, std::memory_order_release);
    gRM->inflight.push({cb, cb->_seqno, writes, std::move(signals), cb->_gpuEventWaits, cb->_channelGeneration,
                        std::move(cpu), quiet, std::exchange(cb->_rangeLoopStamp, 0u), uint32_t(from), uint32_t(to),
                        frameDue},
                       false);
    // A quiet range wakes no one, unless a thread came to wait (waitUntilCompleted) after its end was decided: that
    // waiter's own wake may have found the queue before this push. Push then load here, count then look there, each
    // sequentially consistent: one of the two sees the other.
    if (!quiet || gRM->completionWaiters.load(std::memory_order_seq_cst))
        gRM->inflight.wake();
}

// Its scheduled handlers, taken: run in its queue's order on the queue's handler queue, it scheduled once they ran
static void dispatchScheduled(SwitchMetalCommandBuffer *cb, NSArray *handlers)
{
    if (!handlers.count) {
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::Scheduled);
#endif
        cb->_scheduledDone = true;
        cb->_submitOwner.clear();
        gm20bChanged(cb);
        return;
    }
    cb->_handlersQueued = true;
    // Ownership goes straight from the submit worker to the queue's handler thread, so waitUntilScheduled donates
    // priority across the handlers' queuing and run with no ownerless gap. The worker's own clears after this are
    // clearIf(its port): they leave the handler thread owning it. A waiter asleep on _changes (no owner: held or parked)
    // takes the new owner at the change.
    cb->_submitOwner.setOwner(cb->_queue->_handlerExec.ownerPort());
    gm20bChanged(cb);
    cb->_queue->_handlerExec.enqueue([cb, handlers] {
        @autoreleasepool {
            for (MTLCommandBufferHandler h in handlers)
                h((id<MTLCommandBuffer>)cb);
#if SWITCH_FRAME_TIMELINE
            cb->_frame.mark(switch_frame::Scheduled);
#endif
            cb->_scheduledDone = true;
            cb->_submitOwner.clear();
            gm20bChanged(cb);
        }
    });
}

#if SWITCH_METAL_STATS
// waitUntilScheduled returned for `cb` while its queue waited on a shared event (`returnedUs`), and it is submitted now, or
// finished without: how far its caller ran ahead of the kernel's writers, at most one line a second, with the returns since
static void noteHeldReturn(SwitchMetalCommandBuffer *cb, uint64_t returnedUs, const char *how)
{
    static std::atomic<uint64_t> lastUs{0}, since{0};
    since.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = nowUs();
    uint64_t last = lastUs.load(std::memory_order_relaxed);
    if (now - last < 1000000 || !lastUs.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;
    os_log(gm20bLog(),
           "SwitchMetal: waitUntilScheduled returned held on a shared event: queue %{public}s, %{public}s %llu us "
           "later (%llu "
           "returns since the last line)",
           cb->_queue.label.UTF8String ?: "-", how, (unsigned long long)(now - returnedUs),
           (unsigned long long)since.exchange(0, std::memory_order_relaxed));
}
#endif

// libsyscall's, declared by XNU's bsd/sys/reason.h (both kernel trees), not by the iOS SDK; its string is the crash
// report's termination reason
extern "C" void abort_with_reason(uint32_t reason_namespace, uint64_t reason_code, const char *reason_string,
                                  uint64_t reason_flags) __attribute__((noreturn));
static constexpr uint32_t kOsReasonLibsystem = 18;     // OS_REASON_LIBSYSTEM
static constexpr uint64_t kOsReasonCrashReport = 0x2;  // OS_REASON_FLAG_GENERATE_CRASH_REPORT

// It failed before all its work was submitted, so what it was to present was never wholly rendered: its drawables are
// not presented, as Metal's presentDrawable handler presents only from a command buffer with no error. Dropped, a
// drawable is its own to give back (a CAMetalDrawable's last release returns it to its layer's pool). In the render
// server that is a compositor frame never shown, which no frame may hide: the process ends with the cause (a compositor
// that dies leaves its outstanding frames unshown, and launchd starts it again).
static void withholdPresents(SwitchMetalCommandBuffer *cb)
{
    NSError *error = nil;
    SwitchMetalPred failedBy = PredNone;
    {
        std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
        error = cb->_segError;
        failedBy = SwitchMetalPred(cb->_failedBy);
    }
    cb->_presents = false;
    const char *cause = error.localizedDescription.UTF8String ?: "";
    os_log_error(
        gm20bLog(),
        "SwitchMetal: a command buffer failed before all its work was submitted, so its drawable is not presented: "
        "%{public}s",
        cause);
    if (gRM->raw->role() != gm20b::Device::Role::RenderServer)
        return;
    logFailure(failedBy, error);
    char reason[256];
    snprintf(reason, sizeof reason, "SwitchMetal: the compositor's frame failed in the driver: %s", cause);
    abort_with_reason(kOsReasonLibsystem, 0, reason, kOsReasonCrashReport);
}

static void finishScheduledTurn(SwitchMetalCommandBuffer *cb);

// A queue's command buffers' scheduled handlers run in its order, on its own serial queue: one that waits for another queue's
// command buffer holds up none of that queue's handlers.
void scheduled(SwitchMetalCommandBuffer *cb)
{
    unhold(cb);
    if (cb->_scheduledSent.exchange(true))
        return;  // once (eventHeld: at its commit; then its submission's)
    // an earlier range's failure came in while its last range was submitted
    if (cb->_presents && cb->_segError.set())
        withholdPresents(cb);
#if SWITCH_METAL_STATS
    const uint64_t heldReturnUs = cb->_heldReturnUs.exchange(0, std::memory_order_acq_rel);
#endif
    MTLCommandBufferStatus committed = MTLCommandBufferStatusCommitted;
    cb->_status.compare_exchange_strong(committed, MTLCommandBufferStatusScheduled, std::memory_order_acq_rel,
                                        std::memory_order_relaxed);
    const double now = hostSeconds();
    cb->_kernelEnd = now;
    cb->_gpuStart = now;
    SWITCH_METAL_STAT(cb->_scheduleNotes.scheduledUs.store(nowUs(), std::memory_order_relaxed));
    // Core Animation's render thread waits for this every frame (waitUntilScheduled): with nothing to run, no hop through
    // the handler queue
    dispatchScheduled(cb, gm20bTakeHandlers(cb, cb->_scheduledHandlers, cb->_hasScheduledHandlers));
    finishScheduledTurn(cb);
#if SWITCH_METAL_STATS
    if (heldReturnUs)
        noteHeldReturn(cb, heldReturnUs, "submitted");
#endif
}

// Completed: CPU-signalled events, written resources marked, handlers (on the queue's completion queue when it has one), the
// objects released, waiters woken.
// The driver's empty chunks go back as m2v's blocks do: on memory pressure, and once nothing completed for a second (each
// drain moves the deadline, which the completion thread's own wait carries: GM20BState::idle)
GpuMemoryRelease &gRawRelease = *new GpuMemoryRelease;

static void finished(SwitchMetalCommandBuffer *cb);

void segmentCompleted(SwitchMetalCommandBuffer *cb, NSError *error, const gm20b::RetirementWrites &writes,
                      uint32_t dependencies, const std::vector<SwitchMetalNativeEventSignal> &signals,
                      const std::vector<gm20b::EventTimeline::Promise> &waits, const gm20b::CpuCompletion *cpu)
{
    if (error && ![error.domain isEqualToString:MTLCommandBufferErrorDomain])
        error = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                    code:MTLCommandBufferErrorInternal
                                userInfo:@{
                                    NSLocalizedDescriptionKey : error.localizedDescription ?: @"",
                                    NSUnderlyingErrorKey : error
                                }];
    if (!error && gm20b::eventDependencies(waits, 0, false) != gm20b::EventDependencyState::ready)
        error = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:nil];
    if (!markWritten(writes, !error, cpu) && !error)
        error =
            [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                code:MTLCommandBufferErrorInternal
                            userInfo:@{NSLocalizedDescriptionKey : @"CPU-visible GPU results could not be acquired"}];
    retireEventSignals(signals, !error);
    if (error) {
        std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
        if (!cb->_segError)
            cb->_segError = error;
    }
    const bool last = cb->_finishState.completed();
    if (gRM->progress.progressed())  // a queue head is parked: it is judged again (progress_gate.h)
        wakeSubmit((__bridge void *)cb->_queue, dependencies);
    if (last)
        finished(cb);
}

// A command buffer's completed handlers (`run`): on its queue's completion queue, else on the following threads, behind its
// scheduled handlers where those went to its queue's handler queue; inline on this thread where it has neither handlers nor
// a completion queue. Never on the handler queue itself: a slow completed handler of one frame would hold up the next
// frame's presentDrawable. The queue's turns keep its completed handlers one at a time, in order.
static void deliverCompletion(SwitchMetalCommandBuffer *cb);

static std::vector<SwitchMetalDeferredPresent> takeDeferredPresents(SwitchMetalCommandBuffer *cb, bool &latePresent)
{
    latePresent = false;
    if (!cb->_hasDeferredPresents.load(std::memory_order_seq_cst))
        return {};
    std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
    latePresent = std::exchange(cb->_latePresent, false);
    std::vector<SwitchMetalDeferredPresent> taken;
    taken.swap(cb->_deferredPresents);
    return taken;
}

static void prepareEmptyCompletion(SwitchMetalCommandBuffer *cb)
{
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.finished.fetch_add(1, std::memory_order_relaxed));
    cb->_error = nil;
    cb->_status.store(MTLCommandBufferStatusCompleted, std::memory_order_release);
    const gm20b::CommandBufferTimes t = gm20b::orderedTimes(cb->_kernelStart, cb->_kernelEnd, hostSeconds(), false, 0);
    cb->_kernelStart = t.kernelStart;
    cb->_kernelEnd = t.kernelEnd;
    cb->_gpuStart = t.gpuStart;
    cb->_gpuEnd = t.gpuEnd;
    cb->_completedSent = true;
    cb->_doneHandlers = gm20bTakeHandlers(cb, cb->_completedHandlers, cb->_hasCompletedHandlers);
    bool latePresent = false;
    cb->_donePresents = takeDeferredPresents(cb, latePresent);
    if (latePresent)
        gRM->completionWaiters.fetch_sub(1, std::memory_order_relaxed);
    cb->_doneBehindScheduled = cb->_handlersQueued;
    gm20bChanged(cb);
}

// Its deferred presents and completed handlers run, it is done, and the command buffer behind it that waited for its
// turn is delivered
static void completeBuffer(SwitchMetalCommandBuffer *cb)
{
    SwitchMetalCommandBuffer *cur = cb;
    while (cur) {
        std::vector<SwitchMetalDeferredPresent> presents;
        presents.swap(cur->_donePresents);
        for (const SwitchMetalDeferredPresent &p : presents)
            gm20bRunDeferredPresent(p);
        presents.clear();
        NSArray *handlers = cur->_doneHandlers;
        cur->_doneHandlers = nil;
        for (MTLCommandBufferHandler h in handlers)
            h((id<MTLCommandBuffer>)cur);
        cur->_owners.clear();  // the resources may die now: no submission uses them any more (its capacity is recycled)
        cur->_residencySets = nil;
        cur->_residencySnapshots.clear();
        gm20bBytesDone(cur);
        cur->_computeJobs.clear();
        cur->_sharedEventWaits.clear();
        cur->_resourceReady.reset();
        gm20bRecordingDone(cur);
        cur->_scheduledDone.store(true, std::memory_order_release);
        cur->_completedDone.store(true, std::memory_order_release);
        gm20bClearCompileWaits(cur);
        cur->_submitOwner.clear();
        gm20bChanged(cur);
        gRM->committed.fetch_sub(1, std::memory_order_release);
#if SWITCH_FRAME_TIMELINE
        switch_frame::finished(cur->_frame);
#endif
        void *last = (__bridge void *)cur;
        if (cur->_queue->_last.compare_exchange_strong(last, nullptr, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed))
            CFRelease(last);
        void *next = cur->_turn.finished();
        if (!next)
            return;
        SwitchMetalCommandBuffer *later = (__bridge_transfer SwitchMetalCommandBuffer *)next;
        if (later->_emptyPendingCompletion) {
            later->_emptyPendingCompletion = false;
            prepareEmptyCompletion(later);
        }
        if (gm20b::inlineCompletion(later->_doneBehindScheduled, later->_queue->_completionQueue != nil,
                                    later->_doneHandlers.count != 0, !later->_donePresents.empty())) {
            cur = later;
            continue;
        }
        deliverCompletion(later);
        return;
    }
}

static void scheduledEmpty(SwitchMetalCommandBuffer *cb)
{
    if (cb->_scheduledSent.exchange(true))
        return;
    MTLCommandBufferStatus committed = MTLCommandBufferStatusCommitted;
    cb->_status.compare_exchange_strong(committed, MTLCommandBufferStatusScheduled, std::memory_order_acq_rel,
                                        std::memory_order_relaxed);
    const double now = hostSeconds();
    cb->_kernelEnd = now;
    cb->_gpuStart = now;
    SWITCH_METAL_STAT(cb->_scheduleNotes.scheduledUs.store(nowUs(), std::memory_order_relaxed));
    dispatchScheduled(cb, gm20bTakeHandlers(cb, cb->_scheduledHandlers, cb->_hasScheduledHandlers));
}

static void handEmptyToCompletion(SwitchMetalCommandBuffer *cb)
{
    SwitchMetalCommandBuffer *before = cb->_before;
    cb->_before = nil;
    if (before && !before->_turn.ran()) {
        cb->_emptyPendingCompletion = true;
        void *later = (__bridge_retained void *)cb;
        if (before->_turn.hand(later))
            return;
        (void)(__bridge_transfer SwitchMetalCommandBuffer *)later;
        cb->_emptyPendingCompletion = false;
    }
    prepareEmptyCompletion(cb);
    deliverCompletion(cb);
}

static void drainScheduledTurn(void *next)
{
    while (next) {
        SwitchMetalCommandBuffer *cur = (__bridge_transfer SwitchMetalCommandBuffer *)next;
        scheduledEmpty(cur);
        next = cur->_scheduledTurn.finished();
        handEmptyToCompletion(cur);
    }
}

static void finishScheduledTurn(SwitchMetalCommandBuffer *cb)
{
    drainScheduledTurn(cb->_scheduledTurn.finished());
}

void gm20bFinishEmpty(SwitchMetalCommandBuffer *cb)
{
    cb->_submittedAll.store(true, std::memory_order_release);
    SwitchMetalCommandBuffer *before = cb->_before;
    if (before && !before->_scheduledTurn.ran()) {
        void *later = (__bridge_retained void *)cb;
        if (before->_scheduledTurn.hand(later))
            return;
        (void)(__bridge_transfer SwitchMetalCommandBuffer *)later;
    }
    scheduledEmpty(cb);
    void *next = cb->_scheduledTurn.finished();
    handEmptyToCompletion(cb);
    drainScheduledTurn(next);
}

// dispatch_async_f's work: the command buffer the context holds
static void completeHeld(void *context)
{
    completeBuffer((__bridge_transfer SwitchMetalCommandBuffer *)context);
}

static void completeOnTarget(void *context)
{
    dispatch_queue_t q = ((__bridge SwitchMetalCommandBuffer *)context)->_queue->_completionQueue;
    dispatch_async_f(q ?: followingRootQueue(), context, completeHeld);
}

// Inline where it has neither handlers nor a completion queue nor scheduled handlers queued ahead of them
static void deliverCompletion(SwitchMetalCommandBuffer *cb)
{
    dispatch_queue_t q = cb->_queue->_completionQueue;
    if (gm20b::inlineCompletion(cb->_doneBehindScheduled, q != nil, cb->_doneHandlers.count != 0,
                                !cb->_donePresents.empty())) {
        completeBuffer(cb);
        return;
    }
    void *context = (__bridge_retained void *)cb;
    if (cb->_doneBehindScheduled)
        cb->_queue->_handlerExec.enqueue([context] {
            completeOnTarget(context);
        });
    else
        dispatch_async_f(q ?: followingRootQueue(), context, completeHeld);
}

// What Metal's errors of a GPU address fault carry beneath them (an M4 Max, macOS 27.2.1: a render execute whose range
// named commands far past its indirect command buffer)
static NSError *pageFaultUnderlying()
{
    return [NSError errorWithDomain:@"IOGPUCommandQueueErrorDomain" code:11 userInfo:nil];
}

void failCommandBuffer(SwitchMetalCommandBuffer *cb, NSString *reason, MTLCommandBufferError code, SwitchMetalPred p)
{
    NSError *error = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                         code:code
                                     userInfo:@{NSLocalizedDescriptionKey : reason}];
    // failed as it records: the worker encodes no more of it ahead. One its commit or the worker fails is judged at its
    // turn, which gives back its encode ahead first (gm20bPipelineFailed).
    if (!cb->_committed.load(std::memory_order_relaxed))
        gm20bPipelineVeto(cb, VetoRefused);
    std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
    if (!cb->_segError) {
        cb->_segError = error;
        if (p)
            cb->_failedBy = p;
    }
}

void finish(SwitchMetalCommandBuffer *cb, NSError *error)
{
    if (error) {
        {
            std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
            if (!cb->_segError)
                cb->_segError = error;
        }
        cancelPromises(cb);
    }
    if (cb->_finishState.ready()) {
        finished(cb);
        return;
    }
    // A failed command buffer submits no more: its waitUntilScheduled returns now, not once its earlier work completes
    if (error) {
        gm20bClearCompileWaits(cb);
        cb->_submitOwner.clearIf(gRM->submitThread.load(std::memory_order_relaxed));
        gm20bChanged(cb);
    }
}

// It submits no more and nothing of it is in flight: completed, once (finish_state.h)
static void finished(SwitchMetalCommandBuffer *cb)
{
    NSError *error = nil;
    SwitchMetalPred failedBy = PredNone;
    if (cb->_segError.set()) {
        std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
        error = cb->_segError;
        failedBy = SwitchMetalPred(cb->_failedBy);
    }
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.finished.fetch_add(1, std::memory_order_relaxed));
    SWITCH_METAL_STAT(gm20bNoteFinished(cb));
    cancelPromises(cb);
    releaseEventWaits(cb);
    unhold(cb);
    if (error && cb->_presents && !cb->_submittedAll.load(std::memory_order_acquire))
        withholdPresents(cb);
    // A completed command buffer is scheduled. Its scheduled handlers scheduled() never took run here, before the completed ones:
    // one whose fence passed before its submitting thread's scheduled() (the completion thread wakes on work it was just handed),
    // one that never reached a GPU. Ones scheduled() already queued run first, the completed ones queued behind them on the
    // handler queue.
    const bool sendScheduled = !cb->_scheduledSent.exchange(true);
    NSArray *scheduledHandlers = nil;
    if (sendScheduled) {
        scheduledHandlers = gm20bTakeHandlers(cb, cb->_scheduledHandlers, cb->_hasScheduledHandlers);
        cb->_kernelEnd = hostSeconds();
        SWITCH_METAL_STAT(cb->_scheduleNotes.scheduledUs.store(nowUs(), std::memory_order_relaxed));
#if SWITCH_FRAME_TIMELINE
        SWITCH_METAL_DIAGNOSTIC(gRM->stats.scheduledByFinish++);
#endif
    }
    // counted before its status is published: whoever sees the error sees the count
    if (error)
        commandBufferFailed(failedBy, error);
    {
        cb->_error = error;
        cb->_status.store(error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted,
                          std::memory_order_release);
        // the GPU's own time, its end where this thread saw it complete: the duration exact, both late by that latency
        uint64_t ns = 0;
        const bool stamped = !error && gRM->raw->gpuNanoseconds(cb->_stamp, ns);
        SWITCH_METAL_DIAGNOSTIC(gRM->raw->reportDrawTimes());
        SWITCH_METAL_DIAGNOSTIC(cb->_gpuStamped = stamped);
        const gm20b::CommandBufferTimes t =
            gm20b::orderedTimes(cb->_kernelStart, cb->_kernelEnd, hostSeconds(), stamped, ns);
        cb->_kernelStart = t.kernelStart;
        cb->_kernelEnd = t.kernelEnd;
        cb->_gpuStart = t.gpuStart;
        cb->_gpuEnd = t.gpuEnd;
    }
    cb->_completedSent = true;
    NSArray *handlers = gm20bTakeHandlers(cb, cb->_completedHandlers, cb->_hasCompletedHandlers);
    bool latePresent = false;
    std::vector<SwitchMetalDeferredPresent> presents = takeDeferredPresents(cb, latePresent);
    if (latePresent)
        gRM->completionWaiters.fetch_sub(1, std::memory_order_relaxed);
    if (error) {
        if (!presents.empty() && cb->_presents.load(std::memory_order_acquire))
            withholdPresents(cb);
        presents.clear();
    }
#if SWITCH_METAL_STATS
    const uint64_t heldReturnUs = cb->_heldReturnUs.exchange(0, std::memory_order_acq_rel);
#endif
    if (sendScheduled) {
        dispatchScheduled(cb, scheduledHandlers);
        finishScheduledTurn(cb);
    }
    const bool behindScheduled = cb->_handlersQueued;
    gm20bClearCompileWaits(cb);
    cb->_submitOwner.clearIf(gRM->submitThread.load(std::memory_order_relaxed));
    gm20bChanged(cb);
#if SWITCH_METAL_PROF
    if (cb->_gpuStamped)
        reportLongSpans(cb->_stamp, uint64_t((cb->_gpuEnd - cb->_gpuStart) * 1e9));
#endif
#if SWITCH_METAL_STATS
    if (heldReturnUs)
        noteHeldReturn(cb, heldReturnUs, error ? "failed unsubmitted" : "finished unsubmitted");
#endif
    // A handler may submit and wait for another queue's command buffer, so none runs on a GPU worker. One that waits for a
    // later command buffer of its own queue waits for good, as it does on Metal's serial per-queue completion queue.
    cb->_donePresents = std::move(presents);
    cb->_doneHandlers = handlers;
    cb->_doneBehindScheduled = behindScheduled;
    SwitchMetalCommandBuffer *before = cb->_before;
    cb->_before = nil;
    if (before && !before->_turn.ran()) {
        void *later = (__bridge_retained void *)cb;
        if (before->_turn.hand(later))
            return;
        (void)(__bridge_transfer SwitchMetalCommandBuffer *)later;
    }
    deliverCompletion(cb);
}

// A command buffer the kernel killed the channel of: as Metal reports the GPU's own failures (TegraGM20BAbiFence error:
// 1 MMU fault, 4 timeout), a fault with the description and underlying error an M4 Max's carries
static NSError *killError(uint32_t kernelError)
{
    if (kernelError == 1)
        return [NSError
            errorWithDomain:MTLCommandBufferErrorDomain
                       code:MTLCommandBufferErrorPageFault
                   userInfo:@{
                       NSLocalizedDescriptionKey :
                           @"Caused GPU Address Fault Error (0000000b:kIOGPUCommandBufferCallbackErrorPageFault)",
                       NSUnderlyingErrorKey : pageFaultUnderlying()
                   }];
    const MTLCommandBufferError code = kernelError == 4 ? MTLCommandBufferErrorTimeout : MTLCommandBufferErrorInternal;
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:code userInfo:nil];
}

// The completion thread: one source per device (completion_source.h). It sleeps in the kernel on the first raw range
// in flight whose job interrupts, then retires each range up to it in order, those finding their fence already passed.
// With nothing in flight it sleeps for the next range no later than the idle release's deadline, and gives back there.
void completionLoop()
{
    GM20BState &r = *gRM;
    for (;;) {
        @autoreleasepool {
            std::optional<size_t> target;
            for (;;) {
                gm20b::CompletionLook look;
                r.inflight.waitFor([&](const auto &taken) {
                    look = gm20b::completionLook(taken, r.submitPromised.load(std::memory_order_acquire),
                                                 r.completionWaiters.load(std::memory_order_acquire) != 0,
                                                 clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
                    return look.target.has_value() || look.heldUntil;
                });
                target = look.target;
                if (target)
                    break;
                const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                if (look.heldUntil > now)
                    r.inflight.sleep(dispatch_time(DISPATCH_TIME_NOW, int64_t(look.heldUntil - now)));
            }
            if (*target) {
                const SwitchMetalRawRetirement &cover = r.inflight.at(*target);
                (void)r.raw->wait(cover.seqno, cover.generation);
            }
            SwitchMetalRawRetirement retired = std::move(r.inflight.front());
            SwitchMetalCommandBuffer *cb = retired.buffer;
            // false: the kernel killed the channel with it in flight (a GPU fault or hang); the next command buffer gets a new one
            const bool ok = r.raw->wait(retired.seqno, retired.generation);
            if (!ok && r.raw->lostError() == gm20b::Device::kWaitRefused) {
                os_log_error(gm20bLog(),
                             "SwitchMetal: command buffer %u failed: the kernel kept refusing to wait for it on a "
                             "channel it called "
                             "healthy, so the channel was taken as killed",
                             retired.seqno);
            } else if (!ok && r.raw->lostError() == gm20b::Device::kDependencyFailed) {
                os_log_error(gm20bLog(), "SwitchMetal: command buffer %u failed an upstream dependency", retired.seqno);
            } else if (!ok) {
                const uint64_t va = r.raw->lostFaultVa();
                os_log_error(gm20bLog(),
                             "SwitchMetal: command buffer %u failed: the kernel killed its channel (error %u, fault VA "
                             "0x%llx%s%s)",
                             retired.seqno, r.raw->lostError(), (unsigned long long)va, va ? ": " : "",
                             va ? r.raw->vaOwner(va).c_str() : "");
            }
#if SWITCH_FRAME_TIMELINE
            cb->_frame.mark(switch_frame::Fence);
            switch_frame::trace(switch_frame::kFence, switch_frame::kPoint, cb->_frame.seq);
#endif
            r.inflight.pop();
            NSError *error = ok ? nil : killError(r.raw->lostError());
            if (const gm20b::LoopContinue loop = r.raw->loopContinue(retired.loopStamp); !error && loop.continues) {
                std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
                cb->_loopContinue = {true, loop.tag, retired.loopFrom, retired.loopTo};
            }
            segmentCompleted(cb, error, *retired.writes, SwitchMetalRawProgress, retired.signals, retired.waits,
                             &retired.cpu);
        }
    }
}
