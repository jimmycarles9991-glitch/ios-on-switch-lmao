// SPDX-License-Identifier: MIT
#import "SwitchSharedEvent.h"
#import <objc/runtime.h>

@interface NSObject (SwitchSurfaceEventABI)
- (instancetype)initWithOptions:(NSUInteger)options;
- (instancetype)initWithMachPort:(mach_port_t)port;
- (instancetype)initWithDispatchQueue:(dispatch_queue_t)queue;
- (mach_port_t)eventPort;
- (uint64_t)globalTraceObjectID;
- (BOOL)supportsRollback;
- (uint64_t)signaledValue;
- (void)setSignaledValue:(uint64_t)value;
- (void)notifyListener:(id)listener atValue:(uint64_t)value block:(void (^)(id, uint64_t))block;
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds;
@end

@interface MTLSharedEventHandle (SwitchSurfaceEventHandleABI)
- (instancetype)initWithSharedEvent:(id)event;
- (mach_port_t)eventPort;
- (uint64_t)labelTraceID;
@end

@implementation SwitchSharedEvent {
    id _surfaceEvent;
    uint64_t _traceID;
}
@synthesize label = _label;
@synthesize device = _device;

- (instancetype)initWithOptions:(NSUInteger)options
{
    // The native producer timeline is monotonic. Rollback events need a different dependency contract.
    if (options || !(self = [super init]))
        return nil;
    Class cls = NSClassFromString(@"IOSurfaceSharedEvent");
    _surfaceEvent = [[cls alloc] initWithOptions:options];
    if (!_surfaceEvent || !MACH_PORT_VALID([_surfaceEvent eventPort]))
        return nil;
    _traceID = [_surfaceEvent globalTraceObjectID];
    return self;
}

- (instancetype)initWithMachPort:(mach_port_t)port
{
    if (!MACH_PORT_VALID(port) || !(self = [super init]))
        return nil;
    Class cls = NSClassFromString(@"IOSurfaceSharedEvent");
    _surfaceEvent = [[cls alloc] initWithMachPort:port];
    if (!_surfaceEvent || !MACH_PORT_VALID([_surfaceEvent eventPort]) || [_surfaceEvent supportsRollback])
        return nil;
    _traceID = [_surfaceEvent globalTraceObjectID];
    return self;
}

- (instancetype)initWithHandle:(MTLSharedEventHandle *)handle
{
    if (![handle isKindOfClass:[MTLSharedEventHandle class]])
        return nil;
    self = [self initWithMachPort:handle.eventPort];
    if (self) {
        _traceID = handle.labelTraceID;
        self.label = handle.label;
    }
    return self;
}

- (instancetype)init
{
    return [self initWithOptions:0];
}

- (mach_port_t)eventPort
{
    return [_surfaceEvent eventPort];
}

- (uint64_t)labelTraceID
{
    return _traceID;
}

- (id)IOSurfaceSharedEvent
{
    return _surfaceEvent;
}

- (BOOL)supportsRollback
{
    return NO;
}

- (uint64_t)signaledValue
{
    return [_surfaceEvent signaledValue];
}

- (void)setSignaledValue:(uint64_t)value
{
    [_surfaceEvent setSignaledValue:value];
}

- (MTLSharedEventHandle *)newSharedEventHandle
{
    return [[MTLSharedEventHandle alloc] initWithSharedEvent:self];
}

- (void)notifyListener:(MTLSharedEventListener *)listener
               atValue:(uint64_t)value
                 block:(MTLSharedEventNotificationBlock)block
{
    if (!listener || !block)
        return;
    static char listenerKey;
    id nativeListener = objc_getAssociatedObject(listener, &listenerKey);
    if (!nativeListener) {
        @synchronized(listener) {
            nativeListener = objc_getAssociatedObject(listener, &listenerKey);
            if (!nativeListener) {
                Class cls = NSClassFromString(@"IOSurfaceSharedEventListener");
                nativeListener = [[cls alloc] initWithDispatchQueue:listener.dispatchQueue];
                objc_setAssociatedObject(listener, &listenerKey, nativeListener, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
        }
    }
    [_surfaceEvent notifyListener:nativeListener
                          atValue:value
                            block:^(id, uint64_t reached) {
                                // Registration keeps its listener and public event alive through delivery.
                                (void)nativeListener;
                                block(self, reached);
                            }];
}

- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds
{
    return [_surfaceEvent waitUntilSignaledValue:value timeoutMS:milliseconds];
}

@end
