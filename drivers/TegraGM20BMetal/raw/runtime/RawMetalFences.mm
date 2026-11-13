// SPDX-License-Identifier: MIT
// MTLFence: the device's own object. A fence orders encoders of one queue, and every raw command buffer of the process runs on
// the one raw channel in commit order, its encoders in order, each opening with a drain (a render pass with
// switch_gm20b_pass_barrier, a blit or compute encoder with WAIT_FOR_IDLE and its cache invalidates): a wait is behind its
// update on the GPU with nothing of its own. m2v work between them is ordered by the kernel's Order (RawMetalSubmit.mm).
// Barriers made from a pass's dependencies instead of at every encoder must take each update -> wait pair as one.
#import "RawMetalInternal.h"

@interface RMFence : RMObject <MTLFence>
@end

@implementation RMFence
@dynamic device;  // RMObject's

+ (Class)twinClass
{
    return NSClassFromString(@"NBFence");
}

// Only m2v's replay of an encoder the raw path refused asks for it
- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    return twinPublish(self, _twin, [RM_M2V("a fence's twin") newFence]);
}

@end

@implementation RMDevice (Fences)

- (id<MTLFence>)newFence
{
    RMFence *fence = [RMFence new];
    fence->_device = self;
    return fence;
}

@end
