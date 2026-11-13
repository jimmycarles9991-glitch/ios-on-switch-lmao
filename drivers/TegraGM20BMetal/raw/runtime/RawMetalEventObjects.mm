// SPDX-License-Identifier: MIT
// MTLEvent and MTLSharedEvent: every event the device makes is a kernel event (an IOSurfaceSharedEvent, SwitchSharedEvent.h),
// plain ones too, so the jobs that carry their waits and signals order them on the GPU (raw_event_job.h) and the kernel's
// signal at a job's completion is what wakes their CPU waiters and listeners.
#import "RawMetalInternal.h"
#import "SwitchSharedEvent.h"

static SwitchSharedEvent *made(SwitchSharedEvent *event, id device)
{
    event.device = (id<MTLDevice>)device;
    return event;
}

@implementation RMDevice (Events)

- (id<MTLEvent>)newEvent
{
    return (id<MTLEvent>)made([[SwitchSharedEvent alloc] initWithOptions:0], self);
}

- (id<MTLSharedEvent>)newSharedEvent
{
    return made([[SwitchSharedEvent alloc] initWithOptions:0], self);
}

- (id<MTLSharedEvent>)newSharedEventWithHandle:(MTLSharedEventHandle *)handle
{
    return made([[SwitchSharedEvent alloc] initWithHandle:handle], self);
}

// A kernel timeline only goes forward: an event that can roll back is refused (nil)
- (id<MTLSharedEvent>)newSharedEventWithOptions:(NSInteger)options
{
    return made([[SwitchSharedEvent alloc] initWithOptions:NSUInteger(options)], self);
}

- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)port
{
    return made([[SwitchSharedEvent alloc] initWithMachPort:port], self);
}

@end
