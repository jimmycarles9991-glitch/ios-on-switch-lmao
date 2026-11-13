// SPDX-License-Identifier: MIT
// A compute state made through a completion handler: as the synchronous calls make it (rmComputeState, which answers the
// descriptor's binary archives), on a global queue of the caller's class, which Metal calls the handler on
#import "RawMetalInternal.h"

static dispatch_queue_t computeCreationQueue()
{
    const qos_class_t mine = qos_class_self();
    return dispatch_get_global_queue(mine == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_USER_INITIATED : mine, 0);
}

@implementation RMDevice (ComputeHandlers)

- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)f
                          completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler
{
    RMDevice *me = self;
    dispatch_async(computeCreationQueue(), ^{
        NSError *e = nil;
        id<MTLComputePipelineState> state = [(id<MTLDevice>)me newComputePipelineStateWithFunction:f error:&e];
        handler(state, e);
    });
}

- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                      options:(MTLPipelineOption)o
                            completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
    MTLComputePipelineDescriptor *copy = [d copy];
    RMDevice *me = self;
    dispatch_async(computeCreationQueue(), ^{
        NSError *e = nil;
        MTLAutoreleasedComputePipelineReflection reflection = nil;
        const bool reflect = o & (MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo);
        id<MTLComputePipelineState> state =
            [(id<MTLDevice>)me newComputePipelineStateWithDescriptor:copy
                                                             options:o
                                                          reflection:reflect ? &reflection : nil
                                                               error:&e];
        handler(state, reflection, e);
    });
}

@end
