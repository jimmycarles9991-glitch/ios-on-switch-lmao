// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

@implementation RMCommandBuffer (Events)

- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal
{
    // the m2v device's events, shared or plain (NBEvent answers a shared event's value, set and notifications): the raw path
    // signals them from the CPU once its work completes, and holds its work until a wait is covered (waitCovered); an event
    // without those (another device's) is m2v's alone
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
    [self record:m];
}

- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:false];
}

- (void)encodeSignalEventScheduled:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}

@end

// The committed command buffers that wait for a shared event, until scheduled (heldBehindEvent).
std::mutex gHoldersLock;
std::set<RMCommandBuffer *> gHolders;

bool waitsForEvent(RMCommandBuffer *cb)
{
    for (const Cmd &c : cb->_tape)
        if (c.op == OpWaitEvent)
            return true;
    return false;
}

// Whether a committed command buffer not yet scheduled waits for a shared event value nothing reached yet.
bool heldBehindEvent()
{
    std::lock_guard<std::mutex> l(gHoldersLock);
    for (RMCommandBuffer *cb : gHolders)
        for (const Cmd &c : cb->_tape)
            if (c.op == OpWaitEvent && [(id<MTLSharedEvent>)(__bridge id)c.obj signaledValue] < c.c)
                return true;
    return false;
}

void unhold(RMCommandBuffer *cb)
{
    std::lock_guard<std::mutex> l(gHoldersLock);
    gHolders.erase(cb);
}

uint64_t signalled(NSMapTable *map, void *event)
{
    NSNumber *n = [map objectForKey:(__bridge id)event];
    return n ? n.unsignedLongLongValue : 0;
}

// submit lock held: the event signals `cb` makes, as submitted ones of its driver
void noteSignals(RMCommandBuffer *cb, NSMapTable *map)
{
    for (const Cmd &c : cb->_tape)
        if (c.op == OpSignalEvent && c.c > signalled(map, c.obj))
            [map setObject:@(c.c) forKey:(__bridge id)c.obj];
}

// Whether the shared event wait `i` of `cb` is covered without a CPU wait: signalled already, earlier in `cb`, or (GPU order)
// by a command buffer submitted before it - raw: the channel's own order; m2v: scheduled, so behind the AfterTask acquire.
static bool waitCovered(RMCommandBuffer *cb, size_t i)
{
    const Cmd &c = cb->_tape[i];
    for (size_t k = 0; k < i; k++)
        if (cb->_tape[k].op == OpSignalEvent && cb->_tape[k].obj == c.obj && cb->_tape[k].c >= c.c)
            return true;
    if ([(id<MTLSharedEvent>)(__bridge id)c.obj signaledValue] >= c.c)
        return true;
    return gRM->gpuOrder && (signalled(gRM->rawSignals, c.obj) >= c.c || signalled(gRM->m2vSignals, c.obj) >= c.c);
}

bool eventsReady(RMCommandBuffer *cb)
{
    for (size_t i = 0; i < cb->_tape.size(); i++)
        if (cb->_tape[i].op == OpWaitEvent && !waitCovered(cb, i))
            return false;
    return true;
}

// How long a command buffer waits for a shared event another process signals (waitEvents).
static constexpr int64_t kEventLimitNs = 10 * NSEC_PER_SEC;

// The CPU-ordered remainder: each wait nothing submitted covers, until the event's value is reached (notified, no polling).
void waitEvents(RMCommandBuffer *cb)
{
    for (size_t i = 0; i < cb->_tape.size(); i++) {
        if (cb->_tape[i].op != OpWaitEvent || waitCovered(cb, i))
            continue;
        const Cmd &c = cb->_tape[i];
        once(@"CPU-ordered: a shared event signalled outside this device's submitted work (the host or another "
             @"process): "
             @"the command buffer waiting for it is submitted once it is reached");
        gRM->stats.cpuOrdered++;
        dispatch_semaphore_t reached = dispatch_semaphore_create(0);
        id<MTLSharedEvent> event = (__bridge id)c.obj;
        [event notifyListener:gRM->listener
                      atValue:c.c
                        block:^(id<MTLSharedEvent>, uint64_t) {
                            dispatch_semaphore_signal(reached);
                        }];
        // A signaller that never reaches it (a client killed or suspended mid-frame: Core Animation's per-surface events,
        // CA::OGL::shared_event_submit) holds no one past kEventLimit: the command buffer goes on as if the event were reached,
        // as Core Animation's own SharedEvent::force_complete would make it (r7n: AegirPoster died, r7o: PosterKit's scenes
        // were suspended, and this wait held every later command buffer and the render thread for good).
        if (dispatch_semaphore_wait(reached, dispatch_time(DISPATCH_TIME_NOW, kEventLimitNs))) {
            gRM->stats.eventsReleased++;
            os_log_error(rawLog(),
                         "raw: shared event %p (%{public}s) not reached %llu after %lld ms (at %llu): its command "
                         "buffer goes on without it",
                         (__bridge void *)event, object_getClassName(event), (unsigned long long)c.c,
                         (long long)(kEventLimitNs / 1000000), (unsigned long long)event.signaledValue);
        }
    }
}
