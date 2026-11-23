// SPDX-License-Identifier: MIT
// Selectors the kinds' system images send (drivers/TegraGM20BMetal/abi/kinds/*.json "sent"; harness/selcover) that m2v's objects
// answer zero (the frontend's forwardZero) or do not answer at all, answered here: forwarding them would make an m2v twin of a
// texture, buffer or queue for a zero, or send a command buffer to m2v whole for a no-op.
#import "TegraGM20BMetalInternal.h"

@interface SwitchMetalObject (Answers)
@end
@implementation SwitchMetalObject (Answers)

- (BOOL)isPurgeable
{
    return NO;
}

- (NSUInteger)resourceIndex
{
    return 0;
}

- (void)setResponsibleProcess:(pid_t)pid
{
    (void)pid;
}

- (void)addDebugMarker:(NSString *)marker range:(NSRange)range
{
    (void)marker;
    (void)range;
}

@end

// No statistics, counters or profiling on this device (m2v's queue answers each zero)
@interface SwitchMetalQueue (Answers)
@end
@implementation SwitchMetalQueue (Answers)

- (BOOL)isStatEnabled
{
    return NO;
}

- (void)setStatEnabled:(BOOL)enabled
{
    (void)enabled;
}

- (uint64_t)getStatOptions
{
    return 0;
}

- (void)setStatOptions:(uint64_t)options
{
    (void)options;
}

- (void)setStatLocations:(uint64_t)locations
{
    (void)locations;
}

- (BOOL)isProfilingEnabled
{
    return NO;
}

- (void)setProfilingEnabled:(BOOL)enabled
{
    (void)enabled;
}

- (void)addPerfSampleHandler:(id)handler
{
    (void)handler;
}

- (NSArray *)availableCounters
{
    return nil;
}

- (NSArray *)getRequestedCounters
{
    return nil;
}

- (NSArray *)subdivideCounterList:(NSArray *)counters
{
    (void)counters;
    return nil;
}

@end

// The heap as one use for reading, as SwitchMetalRenderEncoder's
@interface SwitchMetalComputeEncoder (Answers)
@end
@implementation SwitchMetalComputeEncoder (Answers)

- (void)useHeap:(id<MTLHeap>)heap
{
    if ([(id)heap isKindOfClass:[SwitchMetalHeap class]])
        [(id<MTLComputeCommandEncoder>)self useResource:(id<MTLResource>)heap usage:MTLResourceUsageRead];
    else if (heap)
        gm20bCallInvalid(_cb, PredCompute, 0, 0, "a heap used (useHeap:) not made by this device");
}

- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])heaps count:(NSUInteger)count
{
    for (NSUInteger k = 0; k < count; k++)
        [self useHeap:heaps[k]];
}

- (void)setSamplerState:(id<MTLSamplerState>)sampler
            lodMinClamp:(float)lodMin
            lodMaxClamp:(float)lodMax
                atIndex:(NSUInteger)index
{
    [(id<MTLComputeCommandEncoder>)self setSamplerState:clampedSampler(sampler, lodMin, lodMax) atIndex:index];
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])samplers
            lodMinClamps:(const float[])lodMin
            lodMaxClamps:(const float[])lodMax
               withRange:(NSRange)range
{
    for (NSUInteger k = 0; k < range.length; k++)
        [(id<MTLComputeCommandEncoder>)self setSamplerState:clampedSampler(samplers[k], lodMin[k], lodMax[k])
                                                    atIndex:range.location + k];
}

@end

@interface SwitchMetalCommandBuffer (Answers)
@end
@implementation SwitchMetalCommandBuffer (Answers)

- (BOOL)isProfilingEnabled
{
    return NO;
}

- (void)setProfilingEnabled:(BOOL)enabled
{
    (void)enabled;
}

- (NSDictionary *)profilingResults
{
    return nil;
}

- (void)setLogs:(id)logs
{
    (void)logs;
}

@end
