// SPDX-License-Identifier: MIT
// Every encoder's own methods: its trace id, the selectors it answers, its end and fences, and the pool its instances
// come from
#import "TegraGM20BMetalCounters.h"
#import "TegraGM20BMetalInternal.h"
#include "object_pool.h"

// One count for every kind, as IOAccelDeviceGetNextGlobalTraceID is the device's
uint64_t nextTraceId()
{
    static std::atomic<uint64_t> ids{0};
    return ids.fetch_add(1, std::memory_order_relaxed) + 1;
}

@implementation SwitchMetalEncoder

- (uint64_t)globalTraceObjectID
{
    if (!_traceId)
        _traceId = nextTraceId();
    return _traceId;
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

// Tile shading and imageblocks (tile dispatches, tile bindings, imageblock sizes) are Apple4's (Metal feature set tables),
// a family the device does not claim (device_answers.h supportsFamily:, harness/refused.txt); every other Metal
// selector of the claimed families is a method of the encoder. One that reaches here is the caller's error past a NO:
// answered zero, with an error line
static bool tileShading(const char *selector)
{
    for (const char *prefix :
         {"dispatchThreadsPerTile", "setTile", "setImageblockWidth", "setThreadgroupMemoryLength:offset:"})
        if (!strncmp(selector, prefix, strlen(prefix)))
            return true;
    return false;
}

- (void)forwardInvocation:(NSInvocation *)inv
{
    const char *selector = sel_getName(inv.selector);
    if (tileShading(selector))
        SWITCH_REFUSE_DETAIL("gm20b", "call.tile", uintptr_t(inv.selector),
                             "-[%s %s]: tile shading is Apple4's, a family this device does not claim: answered zero",
                             class_getName(object_getClass(self)), selector);
    else
        SWITCH_REFUSE_DETAIL("gm20b", "call.invocation", uintptr_t(inv.selector),
                             "-[%s %s]: no method of this device's encoder: answered zero",
                             class_getName(object_getClass(self)), selector);
    answerZero(self, inv);
}

// One its caller dropped without ending it is ended here, so the tape's encoders stay closed: a check with no message
// (an ended one, every pass's, sends none)
static void endDropped(void *object)
{
    SwitchMetalEncoder *__unsafe_unretained e = (__bridge SwitchMetalEncoder *)object;
    if (!e->_ended && e->_cb)
        [e endEncoding];
}

- (void)dealloc
{
    endDropped((__bridge void *)self);
}

// An end of an encoder that had ended, by its own end or by the begin of the next encoder (SwitchMetalCommandBuffer begin:), is
// Metal's validation error, which an M4 Max with validation off stops the process on (AGX's "endEncoding has already
// been called", "A command encoder is already encoding to this command buffer"): its command buffer fails instead,
// unless it was committed already
static void endedTwice(SwitchMetalCommandBuffer *cb)
{
    if (cb->_committed.load(std::memory_order_relaxed))
        return;
    SWITCH_REFUSE("gm20b", "encoder.ended_twice",
                  "endEncoding on an encoder that had ended, by its own end or by the next encoder's begin: Metal's "
                  "validation error, its command buffer fails MTLCommandBufferErrorInvalidResource");
    failCommandBuffer(cb, @"endEncoding was called on an encoder that had ended", MTLCommandBufferErrorInvalidResource);
}

- (void)endEncoding
{
    SWITCH_API(ApiEncoder);
    if (_ended || !_cb) {
        if (_ended && _cb)
            endedTwice(_cb);
        return;
    }
    _ended = true;
    if (!_counterPending.empty())
        gm20bPlaceCounterMarks(_cb, self, _cb->_plan.size(), false);
    [_cb record:cmd(OpEnd)];
    _cb->_active = nil;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

- (void)insertDebugSignpost:(NSString *)s
{
    (void)s;
}

// (as the command buffer's: everything is resident)
- (void)useResidencySet:(id)set
{
    (void)set;
}

- (void)useResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    (void)sets;
    (void)count;
}

- (void)updateFence:(id<MTLFence>)fence
{
    if (fence && ![(id)fence isKindOfClass:[SwitchMetalObject class]])
        return gm20bCallInvalid(_cb, PredEvent, 0, 0, "updateFence: a fence not made by this device");
    [_cb retain:fence];
    Cmd c = cmd(OpUpdateFence);
    c.obj = (__bridge void *)fence;
    [_cb record:c];
}

- (void)waitForFence:(id<MTLFence>)fence
{
    if (fence && ![(id)fence isKindOfClass:[SwitchMetalObject class]])
        return gm20bCallInvalid(_cb, PredEvent, 0, 0, "waitForFence: a fence not made by this device");
    [_cb retain:fence];
    Cmd c = cmd(OpWaitFence);
    c.obj = (__bridge void *)fence;
    [_cb record:c];
}

@end

void gm20bPoolEncoders()
{
    for (Class cls :
         {[SwitchMetalRenderEncoder class], [SwitchMetalBlitEncoder class], [SwitchMetalComputeEncoder class]})
        if (!gm20bPoolInstances(cls, endDropped))
            os_log_error(gm20bLog(), "SwitchMetal: %{public}s instances are not pooled", class_getName(cls));
}
