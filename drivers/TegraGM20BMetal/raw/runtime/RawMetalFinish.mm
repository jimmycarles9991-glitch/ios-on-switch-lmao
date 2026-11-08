// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// The textures and buffers a command buffer wrote (render targets, copy destinations, and for m2v anything a compute encoder
// bound): their CPU lines are invalidated before the CPU next reads them.
// A recorded object can be m2v's own (a facade command buffer's copy that went to m2v keeps it): only the raw path's carry
// the flag
static void markDirty(void *o)
{
    id object = (__bridge id)o;
    if ([object isKindOfClass:[RMBuffer class]])
        ((RMBuffer *)object)->_gpuDirty = true;
    else if ([object isKindOfClass:[RMTexture class]])
        ((RMTexture *)object)->_gpuDirty = true;
}

void forEachWritten(RMCommandBuffer *cb, size_t from, size_t to, void (*written)(void *))
{
    bool compute = false;
    for (size_t at = from; at < to; at++) {
        const Cmd &c = cb->_tape[at];
        switch (c.op) {
        case OpRender:
            for (const RMPass::Color &a : cb->_passes[c.x].color)
                if (a.texture)
                    written((__bridge void *)a.texture);
            break;
        case OpCompute:
            compute = true;
            break;
        case OpBlit:
            compute = false;
            break;
        case OpEnd:
            compute = false;
            break;
        case OpCopyTexture:
        case OpCopyBufferToTexture:
        case OpMipmaps:
            written(c.op == OpMipmaps ? c.obj : c.obj2);
            break;
        case OpCopyBuffer:
        case OpCopyTextureToBuffer:
            written(c.obj2);
            break;
        case OpFillBuffer:
            written(c.obj);
            break;
        case OpBuffer:
        case OpTexture:
            if (compute)
                written(c.obj);
            break;
        case OpInvocation:  // compute and other recorded calls: every buffer and texture they name
            raw::forEachObject((__bridge NSInvocation *)c.obj, [written](id o) {
                written((__bridge void *)o);
            });
            break;
        default:
            break;
        }
    }
}

// Scheduled: handlers in order on the device's handler queue (never inside commit), then waitUntilScheduled returns.
void scheduled(RMCommandBuffer *cb)
{
    unhold(cb);
    NSArray *handlers;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (cb->_scheduledSent)
            return;  // once (eventHeld: at its commit; then its submission's)
        cb->_scheduledSent = true;
        if (cb->_status == MTLCommandBufferStatusCommitted)
            cb->_status = MTLCommandBufferStatusScheduled;
        handlers = cb->_scheduledHandlers;
        cb->_scheduledHandlers = nil;
        cb->_gpuStart = hostSeconds();
        // Core Animation's render thread waits for this every frame (waitUntilScheduled): with nothing to run, no hop through
        // the handler queue
        if (!handlers.count)
            cb->_scheduledDone = true;
        else  // queued under the lock: a finish() that finds them not done queues its handlers behind them
            dispatch_async(gRM->handlers, ^{
                for (MTLCommandBufferHandler h in handlers)
                    h((id<MTLCommandBuffer>)cb);
                {
                    std::lock_guard<std::mutex> g(cb->_lock);
                    cb->_scheduledDone = true;
                    // a handler added meanwhile ran at once (addScheduledHandler)
                }
                cb->_changed.notify_all();
            });
    }
    if (!handlers.count)
        cb->_changed.notify_all();
}

// Completed: CPU-signalled events, written resources marked, handlers (on the queue's completion queue when it has one), the
// objects released, waiters woken.
// The raw path's empty chunks go back as m2v's blocks do: on memory pressure, and outside the render server once nothing completed
// for NMB_IDLE_RELEASE_MS (each completion moves the deadline)
GpuMemoryRelease &gRawRelease = *new GpuMemoryRelease;

void finish(RMCommandBuffer *cb, NSError *error)
{
    gRM->stats.finished.fetch_add(1, std::memory_order_relaxed);
    unhold(cb);
    forEachWritten(cb, 0, cb->_tape.size(), markDirty);
    if (!cb->_m2v)
        for (const Cmd &c : cb->_tape)
            if (c.op == OpSignalEvent)
                [(id<MTLSharedEvent>)(__bridge id)c.obj setSignaledValue:c.c];
    // A completed command buffer is scheduled. Its scheduled handlers scheduled() never took run here, before the completed ones:
    // one whose fence passed before its submitting thread's scheduled() (the completion thread wakes on work it was just handed),
    // one that never reached a GPU. Ones scheduled() already queued run first, the completed ones queued behind them on the
    // handler queue.
    NSArray *handlers, *scheduledHandlers = nil;
    bool behindScheduled = false;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (!cb->_scheduledSent) {
            scheduledHandlers = cb->_scheduledHandlers;
            cb->_scheduledHandlers = nil;
            cb->_gpuStart = hostSeconds();
#if SWITCH_FRAME_TIMELINE
            gRM->stats.scheduledByFinish++;
#endif
        } else {
            behindScheduled = !cb->_scheduledDone;
        }
        cb->_status = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
        cb->_error = error;
        cb->_gpuEnd = hostSeconds();
        handlers = cb->_completedHandlers;
        cb->_completedHandlers = nil;
        cb->_completedSent = true;
        cb->_scheduledSent = true;  // scheduled() runs no more
    }
    void (^run)(void) = ^{
        for (MTLCommandBufferHandler h in scheduledHandlers)
            h((id<MTLCommandBuffer>)cb);
        for (MTLCommandBufferHandler h in handlers)
            h((id<MTLCommandBuffer>)cb);
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_completedDone = true;
            cb->_scheduledDone = true;
        }
        cb->_changed.notify_all();
        cb->_owners.clear();  // the resources may die now: no submission uses them any more (its capacity is recycled)
        if (gRM->committed.fetch_sub(1, std::memory_order_acq_rel) == 1)
            gRawRelease.drained();
#if SWITCH_FRAME_TIMELINE
        switch_frame::finished(cb->_frame);
#endif
    };
    dispatch_queue_t q = cb->_queue ? cb->_queue->_completionQueue : nil;
    if (behindScheduled)
        dispatch_async(gRM->handlers, ^{
            if (q)
                dispatch_async(q, run);
            else
                run();
        });
    else if (q)
        dispatch_async(q, run);
    else
        run();
}

// The completion thread: sleeps in the kernel on each raw submission's fence (the interrupt), in order.
void completionLoop()
{
    RawState &r = *gRM;
    for (;;) {
        RMCommandBuffer *cb;
        {
            std::unique_lock<std::mutex> l(r.doneLock);
            r.doneChanged.wait(l, [&] {
                return !r.inflight.empty();
            });
            cb = r.inflight.front();
        }
        // false: the kernel killed the channel with it in flight (a GPU fault or hang); the next command buffer gets a new one
        const bool ok = r.raw->wait(cb->_seqno);
        if (!ok && r.raw->lostError() == raw::Device::kWaitRefused) {
            os_log_error(
                rawLog(),
                "raw: command buffer %u failed: the kernel kept refusing to wait for it on a channel it called "
                "healthy, so the channel was taken as killed",
                cb->_seqno);
        } else if (!ok) {
            const uint64_t va = r.raw->lostFaultVa();
            os_log_error(rawLog(),
                         "raw: command buffer %u failed: the kernel killed its channel (error %u, fault VA 0x%llx%s%s)",
                         cb->_seqno, r.raw->lostError(), (unsigned long long)va, va ? ": " : "",
                         va ? r.raw->vaOwner(va).c_str() : "");
        }
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::Fence);
        switch_frame::trace(switch_frame::kFence, switch_frame::kPoint, cb->_frame.seq);
#endif
        {
            std::lock_guard<std::mutex> l(r.doneLock);
            r.inflight.pop_front();
        }
        finish(cb, ok ? nil
                      : [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                            code:MTLCommandBufferErrorInternal
                                        userInfo:nil]);
    }
}
