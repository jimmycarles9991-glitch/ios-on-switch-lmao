// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// ---- the device's submission machinery ----

// A finished command buffer's recording storage, kept for the next one (its capacity a frame's): Core Animation's steady frames
// allocate none of it
struct RMRecording {
    std::vector<Cmd> tape;
    std::vector<RMPass> passes;
    RMBytes bytes;
    std::vector<uint64_t> extra;
    std::vector<id> owners;
};

static std::mutex gRecordingsLock;
static std::vector<RMRecording> gRecordings;

static void takeRecording(RMCommandBuffer *b)
{
    {
        std::lock_guard<std::mutex> l(gRecordingsLock);
        if (!gRecordings.empty()) {
            RMRecording &r = gRecordings.back();
            b->_tape.swap(r.tape);
            b->_passes.swap(r.passes);
            std::swap(b->_bytes, r.bytes);
            b->_extra.swap(r.extra);
            b->_owners.swap(r.owners);
            gRecordings.pop_back();
            return;
        }
    }
    b->_tape.reserve(512);
    b->_owners.reserve(256);
}

static void dropBytes(RMBytes &s)
{
    for (raw::Buffer &k : s.chunks)
        if (k.va)
            gRM->raw->destroyBuffer(k);
        else
            free(k.cpu);
    s = {};
}

static void giveRecording(RMCommandBuffer *b)
{
    constexpr size_t kKept = 4,
                     kKeptBytes = 1 << 20;  // (a command buffer of a few megabytes of setBytes: its own, freed)
    uint64_t bytes = 0;
    for (const raw::Buffer &k : b->_bytes.chunks)
        bytes += k.va ? k.length : kKeptBytes;  // (heap chunks are not kept)
    if (bytes + b->_tape.capacity() * sizeof(Cmd) + b->_extra.capacity() * 8 > kKeptBytes) {
        dropBytes(b->_bytes);
        return;
    }
    RMRecording r;
    r.tape.swap(b->_tape);
    r.passes.swap(b->_passes);
    std::swap(r.bytes, b->_bytes);
    r.extra.swap(b->_extra);
    r.owners.swap(b->_owners);
    r.tape.clear();
    r.passes.clear();
    r.bytes.at = 0;
    r.bytes.used = 0;
    r.extra.clear();
    r.owners.clear();
    {
        std::lock_guard<std::mutex> l(gRecordingsLock);
        if (gRecordings.size() < kKept) {
            gRecordings.push_back(std::move(r));
            return;
        }
    }
    dropBytes(r.bytes);
}

@implementation RMCommandBuffer

- (void)dealloc
{
    giveRecording(self);
}

- (void)retain:(id)object
{
    if (!object || (!_owners.empty() && _owners.back() == object))
        return;
    _owners.push_back(object);
}

- (void)record:(const Cmd &)cmd
{
    _tape.push_back(cmd);
}

// The encoder recorded last: whether it goes to m2v, into its first command
- (void)closeEncoder
{
    if (_encOpen && _encAt < _tape.size())
        _tape[_encAt].index = _encRefused ? 1 : 0;
    _encOpen = false;
}

- (void)takeReason:(RMReason)reason why:(NSString *)why
{
    if (_active)
        _encRefused = true;  // this encoder goes to m2v
    else
        _whole = true;
    if (_reason != WhyRaw)
        return;
    _reason = reason;
    _why = why;
}

// A refusal of the command buffer's own (an event m2v must see, a call on the command buffer): the whole of it goes to m2v
- (void)takeWholeReason:(RMReason)reason why:(NSString *)why
{
    _whole = true;
    [self takeReason:reason why:why];
}

// Answered here, not recorded as invocations (each one sent the whole command buffer to m2v)
- (NSMutableDictionary *)userDictionary
{
    std::lock_guard<std::mutex> l(_lock);
    if (!_userDictionary)
        _userDictionary = [NSMutableDictionary dictionary];
    return _userDictionary;
}

- (uint64_t)globalTraceObjectID
{
    std::lock_guard<std::mutex> l(_lock);
    if (!_traceId)
        _traceId = nextTraceId();
    return _traceId;
}

- (void)useResidencySet:(id)set
{
    _residency.add(&set, 1);
}

// (QuartzCore 27, per command buffer: the kernel charges GPU time to the task that submits it, this one)
- (void)setResponsibleTaskIDs:(const uint32_t *)taskIDs count:(uint32_t)count
{
    (void)taskIDs;
    (void)count;
}

- (void)useResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.add(sets, count);
}

- (id)rmTwin
{
    return nil;
}

- (MTLCommandBufferStatus)status
{
    std::lock_guard<std::mutex> l(_lock);
    return _status;
}

- (NSError *)error
{
    return _error;
}

- (id<MTLCommandQueue>)commandQueue
{
    return (id<MTLCommandQueue>)_queue;
}

- (BOOL)retainedReferences
{
    return YES;
}

- (MTLCommandBufferErrorOption)errorOptions
{
    return MTLCommandBufferErrorOptionNone;
}

- (id<MTLLogContainer>)logs
{
    return nil;
}

- (CFTimeInterval)GPUStartTime
{
    return _gpuStart;
}

- (CFTimeInterval)GPUEndTime
{
    return _gpuEnd;
}

- (CFTimeInterval)kernelStartTime
{
    return _kernelStart;
}

- (CFTimeInterval)kernelEndTime
{
    return _kernelEnd;
}

// SPI Core Animation calls on every command buffer: content protection does not exist on this device (no protected surfaces,
// no secure display), so the options are kept for the getter and change nothing
- (uint64_t)protectionOptions
{
    return _protection;
}

- (void)setProtectionOptions:(uint64_t)options
{
    _protection = options;
}

- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained *)groups count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;  // no cache hints on this GPU
}

- (void)encodeCacheHintFinalize:(uint64_t)tag
                 resourceGroups:(const id __unsafe_unretained *)groups
                          count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    bool now;
    {
        std::lock_guard<std::mutex> l(_lock);
        // once scheduled() took the handlers, one added is run behind them (was: until they ran, it joined a list no one read
        // again, and a presentDrawable added then never presented)
        now = _scheduledSent;
        if (!now) {
            if (!_scheduledHandlers)
                _scheduledHandlers = [NSMutableArray array];
            [_scheduledHandlers addObject:[block copy]];
        }
    }
    if (now)
        dispatch_async(gRM->handlers, ^{
            block((id<MTLCommandBuffer>)self);
        });
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    bool now;
    {
        std::lock_guard<std::mutex> l(_lock);
        now = _completedSent;  // as addScheduledHandler: once finish() took the handlers
        if (!now) {
            if (!_completedHandlers)
                _completedHandlers = [NSMutableArray array];
            [_completedHandlers addObject:[block copy]];
        }
    }
    if (now)
        block((id<MTLCommandBuffer>)self);
}

- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable present];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable presentAtTime:t];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)d
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable presentAfterMinimumDuration:d];
    }];
}

- (void)enqueue
{}

- (void)commit
{
    {
        std::lock_guard<std::mutex> l(_lock);
        if (_committed)
            return;
        _committed = true;
        _status = MTLCommandBufferStatusCommitted;
        _kernelStart = hostSeconds();
    }
#if SWITCH_FRAME_TIMELINE
    static thread_local switch_frame::Since since;
    const raw::Device::Stats &k = gRM->raw->stats;
    switch_frame::committed(_frame, since,
                            {gSurfaceTextures.load(std::memory_order_relaxed),
                             k.importsMade.load(std::memory_order_relaxed),
                             k.importHits.load(std::memory_order_relaxed), gTwinsMade.load(std::memory_order_relaxed)},
                            __builtin_frame_address(0));
#endif
    if (_active)
        [(RMEncoder *)_active endEncoding];
    [(RMDevice *)_device rmCommit:self];
#if SWITCH_FRAME_TIMELINE
    _frame.mark(switch_frame::Back);
#endif
}

- (BOOL)commitAndWaitUntilSubmitted
{
    [self commit];
    [self waitUntilScheduled];
    return YES;
}

- (void)waitUntilScheduled
{
    std::unique_lock<std::mutex> l(_lock);
    if (!_committed || _scheduledDone)
        return;
    const BlockedWait blocked(gRM->stats.scheduledWaits, gRM->stats.scheduledWaitUs);
    if (_changed.wait_for(l, kHeldGrace, [&] {
            return _scheduledDone;
        }))
        return;
    // Behind a shared event no one reached yet (its own wait, or an earlier command buffer's that holds it in order): Metal's
    // command buffer is scheduled there, its GPU waiting for the event, so this returns. Core Animation's render thread flushes
    // with waitUntilScheduled every frame, and forces the per-surface event of a client that died or was suspended after 5 s
    // (CA::WindowServer::SharedEvent::is_complete_with_timeout): blocked here it never could (r7n: AegirPoster died, r7o:
    // PosterKit's scenes were suspended; the display froze for good).
    l.unlock();
    if (heldBehindEvent()) {
        gRM->stats.heldReturns++;
        once(@"waitUntilScheduled of a command buffer behind a shared event not reached yet: returns after 500 ms, as "
             @"Metal's "
             @"(its GPU waits for the event)");
        return;
    }
    l.lock();
    if (!_changed.wait_for(l, kWaitLimit, [&] {
            return _scheduledDone;
        }))
        waitTooLong("waitUntilScheduled of command buffer", (__bridge void *)self);
}

- (void)waitUntilCompleted
{
    std::unique_lock<std::mutex> l(_lock);
    if (!_committed || _completedDone)
        return;
    const BlockedWait blocked(gRM->stats.completedWaits, gRM->stats.completedWaitUs);
    if (!_changed.wait_for(l, kWaitLimit, [&] {
            return _completedDone;
        }))
        waitTooLong("waitUntilCompleted of command buffer", (__bridge void *)self);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's attribution, final once the command buffer completed (its completed handlers may ask): "raw",
// "m2v:<reason>:<first predicate>" or "split:<raw segments>+<m2v segments>:<reason>:<first predicate>"
- (NSString *)switchRawPath
{
    std::lock_guard<std::mutex> l(_lock);
    if (!_completedSent)
        return @"pending";
    if (!_segs.empty()) {
        const auto raw = static_cast<size_t>(std::count_if(_segs.begin(), _segs.end(), [](const RMSegment &s) {
            return s.raw;
        }));
        return [NSString stringWithFormat:@"split:%zu+%zu:%s:%s", raw, _segs.size() - raw, kReasonNames[_reason],
                                          kPredNames[_pred0]];
    }
    if (!_m2v)
        return @"raw";
    return [NSString stringWithFormat:@"m2v:%s:%s", kReasonNames[_reason], kPredNames[_pred0]];
}
#endif

// A selector a command buffer does not answer here: recorded for m2v (the command buffer goes there), answered zero; when m2v's
// command buffer has no method for it either, only answered zero.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector])
        return s;
    Class c = NSClassFromString(@"NBCommandBuffer");
    if (NSMethodSignature *s = c ? [c instanceMethodSignatureForSelector:selector] : nil)
        return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector])
        return YES;
    Class c = NSClassFromString(@"NBCommandBuffer");
    return c && [c instancesRespondToSelector:selector];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
    if (!twinAnswers(NSClassFromString(@"NBCommandBuffer"), inv.selector)) {
        answerZero(self, inv);
        return;
    }
    [inv retainArguments];
    if (!raw::keepPointedArguments(inv))
        SWITCH_REFUSE_DETAIL("raw", "call.pointer", uintptr_t(inv.selector),
                             "-[MTLCommandBuffer %s] recorded with a pointer argument of no known extent",
                             sel_getName(inv.selector));
    [self retain:inv];
    Cmd c = cmd(OpInvocation);
    c.obj = (__bridge void *)inv;
    c.stage = 0xff;  // the command buffer's own, not an encoder's
    [self record:c];
    [self takeWholeReason:WhyCall why:@"a command buffer call recorded as an invocation"];
    hit(self, PredCallCBInvocation, uintptr_t(inv.selector));
    SWITCH_REFUSE_DETAIL("raw", "call.cbinvocation", uintptr_t(inv.selector), "-[MTLCommandBuffer %s] answered zero",
                         sel_getName(inv.selector));
    const NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}

@end

@implementation RMQueue

+ (Class)twinClass
{
    return NSClassFromString(@"NBQueue");
}

- (instancetype)init
{
    if ((self = [super init]))
        queuePriorityAdd(self);
    return self;
}

- (void)dealloc
{
    queuePriorityRemove(self);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    return twinPublish(self, _twin, [gRM->fallback newCommandQueue]);
}

- (id<MTLCommandBuffer>)commandBuffer
{
    SWITCH_API(ApiObject);
    RMCommandBuffer *b = [RMCommandBuffer new];
    b->_device = _device;
    b->_queue = self;
    b->_status = MTLCommandBufferStatusNotEnqueued;
    takeRecording(b);
#if SWITCH_FRAME_TIMELINE
    switch_frame::made(b->_frame);
#endif
    return (id<MTLCommandBuffer>)b;
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];  // retained anyway: lifetimes hang on it (raw_gpu.h)
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary
{}

- (void)addResidencySet:(id)set
{
    _residency.add(&set, 1);
}

- (void)addResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.add(sets, count);
}

- (void)removeResidencySet:(id)set
{
    _residency.remove(&set, 1);
}

- (void)removeResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.remove(sets, count);
}

- (void)setCompletionQueue:(dispatch_queue_t)queue
{
    _completionQueue = queue;
}

- (dispatch_queue_t)completionQueue
{
    return _completionQueue;
}

@end
