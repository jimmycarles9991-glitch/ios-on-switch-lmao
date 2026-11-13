// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalEventJobs.h"

@implementation RMCommandBuffer (Events)

// `milliseconds`: a wait's own limit (encodeWaitForEvent:value:timeout:), 0 for none
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal timeout:(uint32_t)milliseconds
{
    // Plain compatibility events expose this observation contract too. Kernel-backed shared events additionally carry
    // their native capability through the atomic job; opaque events retain a compatibility boundary.
    if (![(id)event respondsToSelector:@selector(signaledValue)] ||
        ![(id)event respondsToSelector:@selector(setSignaledValue:)] ||
        ![(id)event respondsToSelector:@selector(notifyListener:atValue:block:)]) {
        [self takeWholeReason:WhyEvent why:@"an event the raw path cannot read, set or be notified of"];
        hit(self, PredEvent, signal);
    }
    [self retain:event];
    Cmd m = cmd(signal ? OpSignalEvent : OpWaitEvent);
    m.obj = (__bridge void *)event;
    m.c = value;
    m.a = milliseconds;
    [self record:m];
}

- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true timeout:0];
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:false timeout:0];
}

- (void)encodeSignalEventScheduled:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true timeout:0];
}

// The agents a signal is for are Apple's GPU scheduler's: a signal is every agent's here
- (void)encodeSignalEvent:(id<MTLSharedEvent>)event value:(uint64_t)value agentMask:(uint64_t)mask
{
    (void)mask;
    [self event:event value:value signal:true timeout:0];
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value timeout:(uint32_t)milliseconds
{
    [self event:event value:value signal:false timeout:milliseconds];
}

@end

// The committed command buffers that wait for a shared event, until scheduled.
raw::Mutex gHoldersLock;
std::set<RMCommandBuffer *> gHolders;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
std::map<RMCommandBuffer *, RMEventWaitNote> gEventWaits;
#endif

bool waitsForEvent(RMCommandBuffer *cb)
{
    for (const RMPlanEvent &event : cb->_plan.events())
        if (!event.signal)
            return true;
    return false;
}

void unhold(RMCommandBuffer *cb)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (!cb->_inHolders.exchange(false, std::memory_order_acq_rel))
        return;
    std::lock_guard<raw::Mutex> l(gHoldersLock);
    gHolders.erase(cb);
    gEventWaits.erase(cb);
#else
    (void)cb;
#endif
}

void armEventWaits(RMCommandBuffer *cb)
{
    cb->_eventWaits.clear();
    cb->_eventStart = {};
    cb->_eventArmed = 0;
}

raw::EventReadiness eventReadiness(RMCommandBuffer *cb, raw::EventDeadline::Time &nextWake)
{
    using Readiness = raw::EventReadiness;
    if (cb->_resourceReady) {
        const int result = cb->_resourceReady->result();
        if (result == kIOReturnNotReady)
            return Readiness::blocked;
        cb->_resourceReady.reset();
        if (result != KERN_SUCCESS) {
            std::lock_guard<raw::Mutex> lock(cb->_lock);
            cb->_segError = [NSError
                errorWithDomain:MTLCommandBufferErrorDomain
                           code:MTLCommandBufferErrorInternal
                       userInfo:@{NSLocalizedDescriptionKey : @"Resource readiness failed before GPU submission"}];
            return Readiness::ready;
        }
    }
    const bool native =
        cb->_segNext < cb->_segs.size() &&
        (cb->_segs[cb->_segNext].raw || cb->_segs[cb->_segNext].gpuSignal || cb->_segs[cb->_segNext].gpuWait);
    if (!sharedWaitsReady(cb, native))
        return Readiness::blocked;
    if (!cb->_gpuEventWaits.empty()) {
        std::lock_guard<raw::Mutex> lock(cb->_lock);
        switch (raw::eventDependencies(cb->_gpuEventWaits, gRM->raw->channelGeneration(), native)) {
        case raw::EventDependencyState::blocked:
            return Readiness::blocked;
        case raw::EventDependencyState::failed:
            cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                                code:MTLCommandBufferErrorInternal
                                            userInfo:@{NSLocalizedDescriptionKey : @"A GPU event producer failed"}];
            return Readiness::ready;
        case raw::EventDependencyState::ready:
            break;
        }
    }
    if (cb->_segNext >= cb->_segs.size())
        return Readiness::ready;
    const RMSegment &range = cb->_segs[cb->_segNext];
    if (!range.event)
        return Readiness::ready;
    const Cmd c = cb->_plan.legacy(range.from);
    if (c.op == OpSignalEvent) {
        if (range.gpuSignal || signalOrderedOnGpu(cb, range))
            return Readiness::ready;
        std::lock_guard<raw::Mutex> l(cb->_lock);
        return cb->_finishState.pending() ? Readiness::blocked : Readiness::ready;
    }
    id event = (__bridge id)c.obj;
    if ([event signaledValue] >= c.c)
        return Readiness::ready;
    const int shared = sharedEventReadiness(cb, event, c.c, range.gpuWait);
    if (shared > 0)
        return Readiness::ready;
    if (range.gpuWait && shared < 0) {
        cb->_nativeWaitEvent = c.obj;
        raw::EventTimeline::Promise promise;
        {
            std::lock_guard<raw::Mutex> lock(gRM->eventValues);
            promise = gRM->events.observe(reinterpret_cast<uintptr_t>(c.obj), c.c,
                                          reinterpret_cast<uintptr_t>((__bridge void *)cb->_queue),
                                          gRM->raw->channelGeneration());
        }
        if (promise) {
            RAW_DIAGNOSTIC(gRM->stats.waitsOnPromise++);
            std::lock_guard<raw::Mutex> lock(cb->_lock);
            cb->_gpuEventWaits.push_back(std::move(promise));
            return Readiness::ready;
        }
    }
    {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        if (cb->_finishState.pending())
            return Readiness::
                blocked;  // timeout belongs to execution at this position, not time spent on earlier GPU work
    }
    if (cb->_eventArmed != range.from + 1) {
        cb->_eventArmed = range.from + 1;
        RAW_DIAGNOSTIC(gRM->stats.waitsHeldOnCpu++);
        cb->_eventStart = raw::EventDeadline::Clock::now();
        cb->_eventWaits.push_back(range.from);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        {
            std::lock_guard<raw::Mutex> h(gHoldersLock);
            gEventWaits[cb] = {c.obj, c.c, nowUs(), 0};
            cb->_inHolders.store(true, std::memory_order_release);
        }
#endif
        // Registration is done outside the scheduler mutex: an already satisfied event may deliver its notification
        // immediately. The callback does not retain a command buffer or scan unrelated dependency lists.
        void *queue = (__bridge void *)cb->_queue;
        if (shared < 0)
            [event notifyListener:gRM->listener
                          atValue:c.c
                            block:^(id<MTLSharedEvent>, uint64_t) {
                                wakeSubmit(queue);
                            }];
    }
    const raw::EventDeadline deadline(cb->_eventStart, c.a);
    const Readiness state = deadline.state([event signaledValue] >= c.c, raw::EventDeadline::Clock::now());
    if (state == Readiness::blocked)
        nextWake = std::min(nextWake, deadline.until());
    return state;
}

bool promisedEvent(void *event, uint64_t value)
{
    std::lock_guard<raw::Mutex> lock(gRM->eventValues);
    return gRM->events.promised(reinterpret_cast<uintptr_t>(event), value);
}

std::vector<RMNativeEventSignal> publishRawEventSignals(RMCommandBuffer *cb)
{
    std::vector<RMNativeEventSignal> signals;
    std::vector<uintptr_t> wake;
    if (!cb->_rawPrefixGeneration)
        cb->_rawPrefixGeneration = cb->_channelGeneration;
    if (cb->_rawPrefixGeneration != cb->_channelGeneration)
        cb->_gpuPrefixEligible = false;
    if (!cb->_gpuPrefixEligible || cb->_segNext + 1 >= cb->_segs.size() || !cb->_segs[cb->_segNext + 1].event)
        return signals;
    {
        std::lock_guard<raw::Mutex> lock(cb->_lock);
        if (cb->_segError)
            return signals;
        std::lock_guard<raw::Mutex> events(gRM->eventValues);
        for (size_t at = cb->_segNext + 1; at < cb->_segs.size(); ++at) {
            auto &range = cb->_segs[at];
            if (!range.event)
                break;
            const Cmd event = cb->_plan.legacy(range.from);
            if (event.op != OpSignalEvent)
                break;
            if (range.gpuSignal)
                continue;
            auto promise = gRM->events.publish(reinterpret_cast<uintptr_t>(event.obj), event.c,
                                               {cb->_seqno, cb->_channelGeneration}, wake);
            if (!promise)
                break;
            signals.push_back({event.obj, std::move(promise)});
            range.gpuSignal = true;
        }
    }
    for (uintptr_t queue : wake)
        wakeSubmit(reinterpret_cast<void *>(queue));
    return signals;
}

void retireEventSignals(const std::vector<RMNativeEventSignal> &signals, bool success)
{
    for (const auto &signal : signals) {
        std::vector<uintptr_t> wake;
        {
            std::lock_guard<raw::Mutex> lock(gRM->eventValues);
            gRM->events.retire(reinterpret_cast<uintptr_t>(signal.event), signal.promise, success, &wake);
        }
        if (success) {
            id<MTLSharedEvent> event = (__bridge id)signal.event;
            if (event.signaledValue < signal.promise->value)
                event.signaledValue = signal.promise->value;
        }
        for (uintptr_t queue : wake)
            wakeSubmit(reinterpret_cast<void *>(queue));
    }
}

// blocked: a producer still pending on a channel made again since (the encode's begin() may have), whose GPU order this
// channel does not have; its outcome settles it and wakes this queue (eventReadiness parks on it). Only a failed one fails.
raw::EventDependencyState validateNativeEventWaits(RMCommandBuffer *cb, uint64_t generation)
{
    if (cb->_gpuEventWaits.empty())
        return raw::EventDependencyState::ready;
    std::lock_guard<raw::Mutex> lock(cb->_lock);
    const raw::EventDependencyState state = raw::eventDependencies(cb->_gpuEventWaits, generation, true);
    if (state == raw::EventDependencyState::failed)
        cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                            code:MTLCommandBufferErrorInternal
                                        userInfo:@{NSLocalizedDescriptionKey : @"A GPU event producer failed"}];
    return state;
}

bool waitsOnRetiredProducer(RMCommandBuffer *cb)
{
    if (cb->_gpuEventWaits.empty())
        return false;
    std::lock_guard<raw::Mutex> lock(cb->_lock);
    return raw::eventDependencies(cb->_gpuEventWaits, gRM->raw->channelGeneration(), true) ==
           raw::EventDependencyState::blocked;
}

void releaseEventWaits(RMCommandBuffer *cb)
{
    cb->_sharedEventWatch.reset();
    if (!cb->_nativeWaitEvent)
        return;
    std::lock_guard<raw::Mutex> lock(gRM->eventValues);
    gRM->events.unobserve(reinterpret_cast<uintptr_t>(cb->_nativeWaitEvent),
                          reinterpret_cast<uintptr_t>((__bridge void *)cb->_queue));
    cb->_nativeWaitEvent = nullptr;
}

bool expiredEventWait(RMCommandBuffer *cb, size_t index)
{
    const Cmd c = cb->_plan.legacy(index);
    return c.a && cb->_eventArmed == index + 1 &&
           raw::EventDeadline::Clock::now() >= raw::EventDeadline(cb->_eventStart, c.a).until();
}
