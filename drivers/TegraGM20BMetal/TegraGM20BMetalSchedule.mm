// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "scheduled_wait.h"
#include "texel_rows.h"
#include "TegraGM20BJobABI.h"
#import "TegraGM20BMetalPipeline.h"

void wakeSubmit(void *queue, uint32_t dependencies)
{
    gRM->schedule.push({SwitchMetalScheduleChange::Wake, nil, queue, dependencies});
}

// Its place in its queue's order is where the worker takes its enqueue, or its commit where that hands over first: one
// thread's in its call order, and another's after it where the app ordered the calls (the hand-off keeps each push behind
// those that happened before it).
void enqueueBuffer(SwitchMetalCommandBuffer *cb)
{
    uint8_t none = 0;
    if (!cb->_enqueueState.compare_exchange_strong(none, 2, std::memory_order_acq_rel))
        return;
    cb->_before =
        (__bridge_transfer SwitchMetalCommandBuffer *)cb->_queue->_last.exchange((__bridge_retained void *)cb);
    MTLCommandBufferStatus status = MTLCommandBufferStatusNotEnqueued;
    cb->_status.compare_exchange_strong(status, MTLCommandBufferStatusEnqueued);
    gRM->schedule.push({SwitchMetalScheduleChange::Enqueue, cb}, false);  // nothing to submit until its commit
}

bool gm20bClaimCommitSlot(SwitchMetalCommandBuffer *cb)
{
    uint8_t none = 0;
    if (cb->_enqueueState.compare_exchange_strong(none, 1, std::memory_order_acq_rel)) {
        cb->_before =
            (__bridge_transfer SwitchMetalCommandBuffer *)cb->_queue->_last.exchange((__bridge_retained void *)cb);
        return false;
    }
    return none == 2;
}

static void appendImportHandle(const gm20b::Import *import, std::vector<uint64_t> &handles)
{
    if (import && import->id && import->bo.handle &&
        std::find(handles.begin(), handles.end(), import->bo.handle) == handles.end())
        handles.push_back(import->bo.handle);
}

static void appendSurfaceHandles(void *object, const SwitchMetalResourceFacts *facts, std::vector<uint64_t> &handles)
{
    const bool isBuffer = facts ? facts->kind == SwitchMetalResourceKind::Buffer
                                : [(__bridge id)object isKindOfClass:[SwitchMetalBuffer class]];
    if (isBuffer) {
        __unsafe_unretained SwitchMetalBuffer *buffer = (__bridge SwitchMetalBuffer *)object;
        appendImportHandle(buffer->_raw.import, handles);
        return;
    }
    const bool isTexture = facts ? facts->kind == SwitchMetalResourceKind::Texture
                                 : [(__bridge id)object isKindOfClass:[SwitchMetalTexture class]];
    if (!isTexture)
        return;
    __unsafe_unretained SwitchMetalTexture *texture = (__bridge SwitchMetalTexture *)object;
    while (texture->_kind == SwitchMetalTextureView && texture->_parent)
        texture = texture->_parent;
    if (texture->_buffer)
        appendImportHandle(texture->_buffer->_raw.import, handles);
    if (texture->_raw.texelRows)
        appendImportHandle(texture->_raw.texelRows->import, handles);
    for (const gm20b::Texture *plane : {&texture->_raw, texture->_raw.luma, texture->_raw.chroma})
        if (plane)
            appendImportHandle(plane->import, handles);
}

// The imports of the IOSurfaces it writes. Only while the submit thread cannot take it: under the schedule lock, or on
// the submit thread.
static std::vector<uint64_t> writtenSurfaces(SwitchMetalCommandBuffer *cb)
{
    for (const SwitchMetalSegment &range : cb->_segs)
        cb->_plan.prepareRange(range.from, range.to);
    std::vector<uint64_t> handles;
    cb->_plan.forEachWritten(0, cb->_plan.size(), [&](void *object) {
        appendSurfaceHandles(object, cb->_plan.factsOf(object), handles);
    });
    return handles;
}

// Before its waitUntilScheduled may return held (scheduled_wait.h): its writes of IOSurfaces are promised, so a
// present or another process's job that uses one waits for its job on the GPU, as for a scheduled command buffer on
// Apple's GPU. Its last raw range's last job keeps the promises; one made once that range had gone is given back.
static void promiseWrites(SwitchMetalCommandBuffer *cb, const std::vector<uint64_t> &handles)
{
    if (handles.empty())
        return;
    {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        if (cb->_promised.load(std::memory_order_relaxed) || cb->_promisesTaken)
            return;
        cb->_promised.store(true, std::memory_order_release);
    }
    std::vector<uint32_t> made;
    for (size_t at = 0; at < handles.size() && made.size() < TegraGM20BMaxJobPromises; at += TegraGM20BMaxPromiseUses) {
        const std::vector<uint64_t> part(handles.begin() + ptrdiff_t(at),
                                         handles.begin() +
                                             ptrdiff_t(std::min(handles.size(), at + TegraGM20BMaxPromiseUses)));
        uint32_t id = 0;
        if (gRM->raw->promise(part, id))
            made.push_back(id);
        else
            once(
                @"the kernel took no writer promise for surfaces of a command buffer held on a shared event: a present "
                @"may show them before it is submitted");
    }
    bool late;
    {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        late = cb->_promisesTaken;
        if (!late)
            cb->_promises = made;
    }
    if (late)
        for (uint32_t id : made)
            gRM->raw->cancelPromise(id);
}

// finish(): what its last raw range did not carry to the kernel, after all its work completed. One that failed before its
// last range was submitted never makes the IOSurface writes it was to make, and is settled where its failure is decided,
// before its queue goes on to a command buffer that may write the same surfaces: its promises are withdrawn, or with
// none its writes themselves, so a present of one of those surfaces is refused instead of showing the frame before.
// _promised marks it settled: a promise already holds presents off, and unpromised writes set it once withdrawn.
void cancelPromises(SwitchMetalCommandBuffer *cb)
{
    const bool hasError = cb->_segError.set();
    const bool unsubmittedError = hasError && !cb->_submittedAll.load(std::memory_order_acquire);
    if (!unsubmittedError && !cb->_promised.load(std::memory_order_acquire))
        return;
    std::vector<uint32_t> left;
    bool promised;
    {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        promised = cb->_promised.load(std::memory_order_relaxed);
        cb->_promisesTaken = true;
        left.swap(cb->_promises);
    }
    int refusal = KERN_SUCCESS;
    for (uint32_t id : left)
        if (const int kr = gRM->raw->cancelPromise(id, hasError))
            refusal = kr;
    if (unsubmittedError && !promised) {
        const std::vector<uint64_t> written = writtenSurfaces(cb);
        const int kr = written.empty() ? KERN_SUCCESS : gRM->raw->withdrawWrites(written);
        if (kr != KERN_SUCCESS)
            refusal = kr;
        cb->_promised.store(true, std::memory_order_release);
    }
    if (hasError && refusal != KERN_SUCCESS)
        os_log_error(
            gm20bLog(),
            "SwitchMetal: the kernel refused to withdraw the IOSurface writes of a failed command buffer (0x%x): a "
            "present of one may show the frame before",
            unsigned(refusal));
}

// Method 34 Arm: the kernel holds the bound on its unkept writer promises too, since Metal's bound is its kernel's and
// fires whatever this process is doing, suspended or stopped; a reader of the surfaces then goes on at `due` without
// waiting for this worker to run. `fails`: the command buffer fails at `due` (an untimed wait's own bound), so the
// kernel refuses a later keep. On the worker: it arms before it keeps, on one thread.
static void armBound(SwitchMetalCommandBuffer *cb, gm20b::EventDeadline::Time due, bool fails = false)
{
    uint32_t mask = 0;
    {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        for (uint32_t id : cb->_promises)
            mask |= 1u << id;
    }
    if (!mask || due == gm20b::EventDeadline::Time{})
        return;
    const int64_t left =
        std::chrono::duration_cast<std::chrono::nanoseconds>(due - gm20b::EventDeadline::now()).count();
    if (const int kr = gRM->raw->armPromises(mask, uint64_t(std::max<int64_t>(left, 0)), fails))
        once([NSString
            stringWithFormat:@"the kernel took no bound for the writer promises of a command buffer held on a "
                             @"shared event (0x%x): a reader of its surfaces waits for this process to run",
                             kr]);
}

// The head held at a wait on a shared event: its own bound (`fails`: an untimed wait's, where it fails), and one a bound
// later for every command buffer held behind it, which this worker runs once the head fails or goes on (on Apple's GPU
// a held buffer behind another fails 5 s after it)
static gm20b::EventDeadline::Time holdBound(SwitchMetalCommandBuffer *cb, bool &fails)
{
    const Cmd c = cb->_plan.legacy(cb->_segs[cb->_segNext].from);
    const gm20b::EventDeadline wait(cb->_eventStart, uint32_t(c.a));
    fails = !c.a;
    cb->_queue->_heldBound = wait.holderBound() + gm20b::EventDeadline::kHeldBound;
    return wait.holderBound();
}

static bool renderPassReachesGpu(const SwitchMetalPass &pass)
{
    for (const SwitchMetalPass::Color &a : pass.color)
        if (a.texture && (a.load == MTLLoadActionClear || a.store != MTLStoreActionDontCare || a.resolve))
            return true;
    for (const SwitchMetalPass::Zeta &z : pass.zeta)
        if (z.texture && (z.load == MTLLoadActionClear || z.store != MTLStoreActionDontCare || z.resolve))
            return true;
    return pass.depthResolve != nil || pass.stencilResolve != nil;
}

static bool canFastPathEmpty(SwitchMetalCommandBuffer *cb)
{
    if (cb->_enqueueState.load(std::memory_order_acquire) == 2 || cb->_segError.set() || cb->_handedOnce ||
        cb->_predicates.recorded() || !cb->_counterMarks.empty())
        return false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (!cb->_probeOps.empty())
        return false;
#endif
    const SwitchMetalCommandPlan &plan = cb->_plan;
    if (plan.recordingError() != SwitchMetalPlanError::None)
        return false;
    for (size_t at = 0; at < plan.size();) {
        const Op beginOp = plan.operation(at).op;
        if (!SwitchMetalCommandPlan::isEncoderBegin(beginOp))
            return false;
        const size_t end = plan.encoderEnd(at);
        if (end < at + 2 || end > plan.size() || plan.operation(end - 1).op != OpEnd)
            return false;
        if (beginOp == OpRender && renderPassReachesGpu(cb->_passes[plan.legacy(at).x]))
            return false;
        for (size_t i = at + 1; i + 1 < end; ++i) {
            const Op op = plan.operation(i).op;
            const bool noOp = op == OpUpdateFence || op == OpWaitFence || (beginOp == OpRender && op == OpBarrier) ||
                              (beginOp == OpCompute && op == OpKernelBarrier);
            if (!noOp)
                return false;
        }
        at = end;
    }
    return true;
}

void commitBuffer(SwitchMetalCommandBuffer *cb)
{
    gm20b::followers().committing();
    GM20BState &r = *gRM;
    if (canFastPathEmpty(cb)) {
        [cb closeEncoder];
        r.committed.fetch_add(1, std::memory_order_relaxed);
        gm20bFinishEmpty(cb);
        return;
    }
    planSegments(cb);
    r.committed.fetch_add(1);
    if (r.raw->purgedArenas())
        restoreArenas(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (waitsForEvent(cb)) {
        std::lock_guard<gm20b::Mutex> h(gHoldersLock);
        gHolders.insert(cb);
        cb->_inHolders.store(true, std::memory_order_release);
    }
#endif
    armEventWaits(cb);
    SWITCH_METAL_DIAGNOSTIC(r.stats.deferred++);
    if (const uint32_t submitter = r.submitThread.load(std::memory_order_acquire))
        cb->_submitOwner.setOwner(submitter);
    r.schedule.push({SwitchMetalScheduleChange::Commit, cb});
}

static thread_local bool tSubmitWorker = false;

bool gm20bOnSubmitWorker()
{
    return tSubmitWorker;
}

// The worker's, between its submissions: a move begins and commits the device's one command buffer on its own, so an
// encode ahead holding it goes back first
static void runMove(const std::function<void()> &work, bool yieldAll)
{
    if (yieldAll)
        gm20bPipelineYield(nil, SIZE_MAX, false);
    work();
}

void gm20bRunOnSubmitWorker(const std::function<void()> &work)
{
    if (tSubmitWorker) {
        runMove(work, true);
        return;
    }
    SwitchMetalMove move{&work, dispatch_semaphore_create(0), true};
    SwitchMetalScheduleChange change;
    change.kind = SwitchMetalScheduleChange::Move;
    change.move = &move;
    gRM->schedule.push(std::move(change));
    dispatch_semaphore_wait(move.done, DISPATCH_TIME_FOREVER);
}

void gm20bReleaseAhead(bool yieldOpen)
{
    const std::function<void()> work = [yieldOpen] {
        @autoreleasepool {
            gm20bPipelineOrphans(yieldOpen);
        }
    };
    if (tSubmitWorker) {
        work();
        return;
    }
    SwitchMetalMove move{&work, dispatch_semaphore_create(0), false};
    SwitchMetalScheduleChange change;
    change.kind = SwitchMetalScheduleChange::Move;
    change.move = &move;
    gRM->schedule.push(std::move(change));
    dispatch_semaphore_wait(move.done, DISPATCH_TIME_FOREVER);
}

struct EarlyPromisedEntry {
    void *cb = nullptr;
    std::vector<uint64_t> handles;
};

static std::vector<EarlyPromisedEntry> &earlyPromised()
{
    static auto *entries = new std::vector<EarlyPromisedEntry>;
    return *entries;
}

static bool hasEarlyPromisedHandle(const std::vector<uint64_t> &handles)
{
    for (const EarlyPromisedEntry &entry : earlyPromised())
        for (uint64_t h : handles)
            if (std::find(entry.handles.begin(), entry.handles.end(), h) != entry.handles.end())
                return true;
    return false;
}

static void releaseEarlyPromised(SwitchMetalCommandBuffer *cb)
{
    if (!cb->_earlyPromised)
        return;
    cb->_earlyPromised = false;
    auto &entries = earlyPromised();
    void *key = (__bridge void *)cb;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].cb == key) {
            entries[i] = std::move(entries.back());
            entries.pop_back();
            return;
        }
}

static void markQueueScheduled(SwitchMetalCommandBuffer *cb)
{
    if (!cb->_countedUnscheduled)
        return;
    cb->_countedUnscheduled = false;
    if (cb->_queue->_unscheduled)
        --cb->_queue->_unscheduled;
}

static void earlyWrittenSurfaces(SwitchMetalCommandBuffer *cb, std::vector<uint64_t> &handles)
{
    handles.clear();
    const SwitchMetalCommandPlan &plan = cb->_plan;
    plan.forEachOperationUse(0, plan.size(), [&](void *object, uint8_t access) {
        if (access & gm20b::ResourceUse::Write)
            appendSurfaceHandles(object, plan.factsOf(object), handles);
    });
}

static bool promiseWritesEarly(SwitchMetalCommandBuffer *cb, const std::vector<uint64_t> &handles)
{
    if (handles.empty())
        return true;
    if (handles.size() > size_t(TegraGM20BMaxJobPromises) * TegraGM20BMaxPromiseUses)
        return false;
    {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        if (cb->_promised.load(std::memory_order_relaxed) || cb->_promisesTaken)
            return false;
    }
    std::vector<uint32_t> made;
    for (size_t at = 0; at < handles.size(); at += TegraGM20BMaxPromiseUses) {
        const std::vector<uint64_t> part(handles.begin() + ptrdiff_t(at),
                                         handles.begin() +
                                             ptrdiff_t(std::min(handles.size(), at + TegraGM20BMaxPromiseUses)));
        uint32_t id = 0;
        if (!gRM->raw->promise(part, id)) {
            for (uint32_t prior : made)
                gRM->raw->cancelPromise(prior, false);
            return false;
        }
        made.push_back(id);
    }
    bool late = false;
    {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        late = cb->_promisesTaken || cb->_promised.load(std::memory_order_relaxed);
        if (!late) {
            cb->_promises = std::move(made);
            cb->_promised.store(true, std::memory_order_release);
        }
    }
    if (late) {
        for (uint32_t id : made)
            gRM->raw->cancelPromise(id, false);
        return false;
    }
    return true;
}

static bool tryEarlySchedule(GM20BState &r, SwitchMetalCommandBuffer *cb, bool queueHead, bool workerBusy,
                             bool readyChecked)
{
    if (r.raw->role() != gm20b::Device::Role::RenderServer)
        return false;
    if (cb->_scheduledSent.load(std::memory_order_acquire) || cb->_held.load(std::memory_order_acquire) ||
        cb->_segError.set() || cb->_segs.empty() || cb->_predicates.recorded())
        return false;
    const bool allChecked = readyChecked && cb->_segs.size() == 1;
    const bool variantPending = (cb->_pipelineVeto.load(std::memory_order_acquire) & VetoVariant) != 0;
    if (!allChecked && (!cb->_computeJobs.empty() || variantPending))
        return false;
    for (const SwitchMetalSegment &seg : cb->_segs)
        if (seg.event || seg.held)
            return false;
    const uint8_t enqueueState = cb->_enqueueState.load(std::memory_order_acquire);
    if (!queueHead && (cb->_queue->_unscheduled != 1 || enqueueState == 2))
        return false;
    if (enqueueState == 1 && cb->_before && !cb->_before->_scheduledTurn.ran())
        return false;
    uint32_t remaining = 0;
    bool multiRange = false;
    gm20bPipelineEarlyWork(cb, gm20b::kEarlySchedulePasses, remaining, multiRange);
    if (!gm20b::earlySchedule(true, workerBusy, remaining, multiRange))
        return false;
    static thread_local std::vector<uint64_t> handles;
    earlyWrittenSurfaces(cb, handles);
    if (!handles.empty() && hasEarlyPromisedHandle(handles))
        return false;
    if (!promiseWritesEarly(cb, handles))
        return false;
    if (!handles.empty()) {
        earlyPromised().push_back({(__bridge void *)cb, handles});
        cb->_earlyPromised = true;
    }
    scheduled(cb);
    markQueueScheduled(cb);
    return true;
}

// A change another thread handed the worker, on the worker
static void applyChange(GM20BState &r, const SwitchMetalScheduleChange &change)
{
    SwitchMetalCommandBuffer *cb = change.buffer;
    if (change.kind == SwitchMetalScheduleChange::Encoded) {
        gm20bPipelineHanded(cb);
        return;
    }
    if (change.kind == SwitchMetalScheduleChange::Move) {
        runMove(*change.move->work, change.move->yieldAll);
        dispatch_semaphore_signal(change.move->done);
        return;
    }
    // a commit on one thread can hand over before the enqueue another thread made of the same command buffer
    if (change.kind != SwitchMetalScheduleChange::Wake && !cb->_reserved) {
        cb->_reservation = r.waiting.reserve((__bridge void *)cb->_queue, cb);
        cb->_reserved = true;
        cb->_countedUnscheduled = true;
        ++cb->_queue->_unscheduled;
    }
    switch (change.kind) {
    case SwitchMetalScheduleChange::Enqueue:
        break;
    case SwitchMetalScheduleChange::Commit: {
        bool held = false;
        r.waiting.commit((__bridge void *)cb->_queue, cb, cb->_reservation, &held);
        if (held) {
            promiseWrites(cb, writtenSurfaces(cb));
            cb->_held = true;
            armBound(cb, cb->_queue->_heldBound);
            cb->_submitOwner.clear();
            gm20bChanged(cb);
        } else {
            const bool workerBusy = r.waiting.committed() > 1 || r.waiting.parked() != 0;
            tryEarlySchedule(r, cb, false, workerBusy, false);
        }
        break;
    }
    case SwitchMetalScheduleChange::Wake:
        if (change.queue)
            r.waiting.wake(change.queue);
        r.waiting.wakeDependencies(change.dependencies);
        break;
    case SwitchMetalScheduleChange::Encoded:
    case SwitchMetalScheduleChange::Move:
        break;
    }
}

static void applyChanges(GM20BState &r)
{
    while (SwitchMetalScheduleChange *change = r.schedule.peek()) {
        applyChange(r, *change);
        r.schedule.pop();
    }
}

// A native variant this queue head waits for, made on the raw compiler's two bounded workers: refused, its command buffer
// fails. No deadline.
// Work the driver cannot run: its command buffer fails, as Metal's does for a pipeline it could not compile, never handed
// to another device.
static NSError *refusedAtReadiness(const char *why)
{
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain
                               code:MTLCommandBufferErrorInternal
                           userInfo:@{NSLocalizedDescriptionKey : [NSString stringWithFormat:@"%s", why ?: ""]}];
}

static int variantReadiness(SwitchMetalCommandBuffer *cb, const SwitchMetalSegment &range, const char **why,
                            std::vector<std::function<bool()>> &compileWaits)
{
    using Variant = gm20b::Variant<gm20b::Pipeline>;
    bool pending = false;
    for (void *object : cb->_plan.renderVariants(range.from, range.to)) {
        auto *variant = static_cast<Variant *>(object);
        std::function<void()> observer;
        const bool firstSeen = cb->_variantObserved.insert(object).second;
        if (firstSeen) {
            void *queue = (__bridge void *)cb->_queue;
            observer = [queue] {
                wakeSubmit(queue);
            };
        }
        const auto state = variant->observe(std::move(observer));
        if (state == Variant::State::Failed)
            return -2;
        if (state == Variant::State::Refused) {
            *why = "a render state variant the shader compiler refused";
            return -1;
        }
        if (state == Variant::State::Pending) {
            pending = true;
            if (firstSeen) {
                __weak SwitchMetalCommandBuffer *weakCb = cb;
                variant->observeWait([weakCb] {
                    if (SwitchMetalCommandBuffer *strong = weakCb)
                        gm20bNoteCompileWaitReady(strong);
                });
            }
            if (auto keep = variant->weak_from_this().lock())
                compileWaits.push_back([keep = std::move(keep)] {
                    return keep->waitPending();
                });
        }
    }
    return pending ? 0 : 1;
}

// What a head eventReadiness found blocked waits for: its own earlier work, a value a submitted command buffer promised, or a
// shared event value nothing reached yet
static gm20b::HeadWait eventWait(SwitchMetalCommandBuffer *cb)
{
    {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        if (cb->_finishState.pending() || cb->_resourceReady)
            return gm20b::HeadWait::signal;
    }
    if (cb->_segNext >= cb->_segs.size() || !cb->_segs[cb->_segNext].event)
        return gm20b::HeadWait::signal;
    const Cmd c = cb->_plan.legacy(cb->_segs[cb->_segNext].from);
    if (c.op != OpWaitEvent)
        return gm20b::HeadWait::signal;
    return promisedEvent(c.obj, c.c) ? gm20b::HeadWait::promised : gm20b::HeadWait::event;
}

// The command buffers a hold took promise their writes, and those behind the held head get their bound; `head`, the held
// queue's head, has its own, which the submit loop arms
static void markHeld(std::vector<SwitchMetalCommandBuffer *> &buffers, bool held, SwitchMetalCommandBuffer *head = nil)
{
    for (SwitchMetalCommandBuffer *cb : buffers) {
        if (held)
            promiseWrites(cb, writtenSurfaces(cb));
        cb->_held = held;
        if (held && cb != head)
            armBound(cb, cb->_queue->_heldBound);
        if (held) {
            cb->_submitOwner.clear();
            gm20bChanged(cb);
        }
    }
    buffers.clear();
}

// The submit thread has no more raw work in hand: the quiet ranges it left in flight wait for their own interrupt. Only
// this thread stores the promise (retireRawRange).
static void closePromise()
{
    if (!gRM->submitPromised.load(std::memory_order_relaxed))
        return;
    gRM->submitPromised.store(false, std::memory_order_release);
    gRM->inflight.wake();
}

void submitLoop()
{
    GM20BState &r = *gRM;
    tSubmitWorker = true;
    const uint32_t selfPort = gm20b::OwnedWord::currentOwner();
    r.submitThread.store(selfPort, std::memory_order_release);
    std::vector<SwitchMetalCommandBuffer *> marked;
    for (;;) {
        @autoreleasepool {
            gm20b::SubmitQueue<void *, SwitchMetalCommandBuffer *>::Ticket ticket;
            bool followed = false;
            for (;;) {
                applyChanges(r);
                r.waiting.expire(gm20b::EventDeadline::now());
                r.waitingCommitted.store(uint32_t(r.waiting.committed()), std::memory_order_relaxed);
                ticket = r.waiting.next();
                if (ticket) {
                    followed = r.waiting.ready() || r.waiting.behind(ticket);
                    break;
                }
                closePromise();
                // nothing to submit: what the command buffer being recorded has published, encoded ahead of its commit
                if (gm20bPipelineStep())
                    continue;
                r.progress.idle(r.waiting.parked() != 0);
                const auto due = r.waiting.deadline();
                dispatch_time_t until = DISPATCH_TIME_FOREVER;
                if (due != gm20b::EventDeadline::Time::max()) {
                    const auto left = due - gm20b::EventDeadline::now();
                    until = dispatch_time(
                        DISPATCH_TIME_NOW,
                        std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count()));
                }
                r.submitAsleep.store(true, std::memory_order_seq_cst);
                if (gm20bPipelinePublished()) {
                    r.submitAsleep.store(false, std::memory_order_relaxed);
                    continue;
                }
                r.schedule.sleep(until);
                r.submitAsleep.store(false, std::memory_order_relaxed);
            }
            const uint64_t judged = r.progress.visit();
            SwitchMetalCommandBuffer *cb = ticket.value;
            gm20bClearCompileWaits(cb);
            if (!cb->_scheduledSent.load(std::memory_order_acquire) && !cb->_held.load(std::memory_order_acquire) &&
                !cb->_submitOwner.hasOwner()) {
                cb->_submitOwner.setOwner(selfPort);
                gm20bChanged(cb);
            }
            SWITCH_METAL_DIAGNOSTIC(gm20bNoteTaken(cb));
            auto deadline = gm20b::EventDeadline::Time::max();
            uint32_t dependencies = 0;
            bool blocked = false;
            gm20b::HeadWait wait = gm20b::HeadWait::signal;
            std::vector<std::function<bool()>> compileWaits;
            const auto failed = [cb]() -> NSError * {
                if (!cb->_segError.set())
                    return nil;
                std::lock_guard<gm20b::Mutex> l(cb->_lock);
                return cb->_segError;
            };
            NSError *error = failed();
            if (!error) {
                blocked = eventReadiness(cb, deadline) == gm20b::EventReadiness::blocked;
                if (blocked)
                    wait = eventWait(cb);
                error = failed();
            }
            if (!error && !blocked && cb->_segNext < cb->_segs.size()) {
                gm20bPipelineSplit(cb);
                SwitchMetalSegment &range = cb->_segs[cb->_segNext];
                cb->_plan.prepareRange(range.from, range.to);
                // the completion's _loopContinue is written before its range's completed() releases the count
                const bool loopRetiring = range.held && cb->_finishState.pending() != 0;
                if (loopRetiring) {
                    blocked = true;
                    dependencies = SwitchMetalRawProgress;
                } else if (!range.event) {
                    const char *why = "";
                    int ready = variantReadiness(cb, range, &why, compileWaits);
                    if (ready > 0)
                        ready = computeReadiness(cb, range, &why, compileWaits);
                    if (ready == -2) {
                        error = [NSError
                            errorWithDomain:MTLCommandBufferErrorDomain
                                       code:MTLCommandBufferErrorInternal
                                   userInfo:@{NSLocalizedDescriptionKey : @"A render pipeline could not be compiled"}];
                    } else if (ready < 0) {
                        error = refusedAtReadiness(why);
                    } else if (!ready) {
                        blocked = true;
                        wait = gm20b::HeadWait::compile;
                    }
                }
            }
            if (blocked) {
                if (!r.progress.mayPark(judged)) {  // a completion came since it was judged
                    r.waiting.yield(ticket);
                    continue;
                }
                r.waiting.park(ticket, dependencies, deadline);
                if (wait == gm20b::HeadWait::compile)
                    gm20bSetCompileWaits(cb, std::move(compileWaits));
                else
                    cb->_submitOwner.clear();
                SWITCH_METAL_DIAGNOSTIC(gm20bNoteParked(cb, unsigned(wait)));
                if (!gm20b::holds(wait)) {
                    markHeld(marked, true);
                    continue;
                }
                r.waiting.hold(ticket.key, marked);
                bool fails = false;
                const auto bound = holdBound(cb, fails);
                markHeld(marked, true, cb);
                if (cb->_boundArmed != cb->_eventArmed) {
                    cb->_boundArmed = cb->_eventArmed;
                    armBound(cb, bound, fails);
                }
                continue;
            }
            bool submitted = true;
            if (error) {
                gm20bPipelineFailed(cb);
                finish(cb, error);
            } else {
                if (!cb->_segNext)
                    tryEarlySchedule(r, cb, true, followed, true);
                submitted = submitRangeExclusive(cb, followed);
            }
            r.waiting.release(ticket.key, marked);
            if (submitted) {
                cb->_submitOwner.clear();
                r.waiting.retire(ticket);
                markQueueScheduled(cb);
                releaseEarlyPromised(cb);
            } else
                r.waiting.yield(ticket);
            markHeld(marked, false);
        }
    }
}
