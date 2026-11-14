// SPDX-License-Identifier: MIT
// MTLFence: the device's own object. A fence orders encoders of one queue, and every raw command buffer of the process runs on
// the one raw channel in commit order, its encoders in order, each opening with a drain (a render pass with
// switch_gm20b_pass_barrier, a blit or compute encoder with WAIT_FOR_IDLE and its cache invalidates) or a change of engine,
// which waits for the engine left behind (raw_barrier_forms.h): a wait is behind its update on the GPU with nothing of its
// own. A pass opens with the pixel barrier alone only where its recording names everything it reads, and one that waits
// for a fence is not one (RawMetalPassUses.h). A command buffer's first encoder opens with nothing: the one before it on
// the channel ended with its fence's RELEASE_WFI (raw_event_release.h), and its entry invalidates what it reads. m2v work between them is ordered by the kernel's Order (RawMetalSubmit.mm).
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
    return twinPublish(self, _twin, [RM_M2V(NeedFenceTwin) newFence]);
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
