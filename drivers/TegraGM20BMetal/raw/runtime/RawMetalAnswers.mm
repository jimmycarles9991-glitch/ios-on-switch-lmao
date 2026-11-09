// SPDX-License-Identifier: MIT
// Selectors the kinds' system images send (drivers/NativeMetal/abi/kinds/*.json "sent"; raw/test/selcover) that m2v's objects
// answer zero (the frontend's forwardZero) or do not answer at all, answered here: forwarding them would make an m2v twin of a
// texture, buffer or queue for a zero, or send a command buffer to m2v whole for a no-op.
#import "RawMetalInternal.h"

@interface RMObject (Answers)
@end
@implementation RMObject (Answers)

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

static bool faceWritesStencil(const raw::StencilFace &f)
{
    return f.writeMask && (f.fail || f.depthFail || f.pass);
}

@interface RMDepthStencil (Answers)
@end
@implementation RMDepthStencil (Answers)

- (BOOL)readsDepth
{
    return _raw.depthCompare != MTLCompareFunctionAlways;
}

- (BOOL)writesDepth
{
    return _raw.depthWrite;
}

- (BOOL)readsStencil
{
    return _raw.front.compare != MTLCompareFunctionAlways || _raw.back.compare != MTLCompareFunctionAlways;
}

- (BOOL)writesStencil
{
    return faceWritesStencil(_raw.front) || faceWritesStencil(_raw.back);
}

@end

// No statistics, counters or profiling on this device (m2v's queue answers each zero)
@interface RMQueue (Answers)
@end
@implementation RMQueue (Answers)

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

// The facade's heaps hold ordinary resources: nothing to make resident (RMRenderEncoder's are the same)
@interface RMComputeEncoder (Answers)
@end
@implementation RMComputeEncoder (Answers)

- (void)useHeap:(id<MTLHeap>)heap
{
    (void)heap;
}

- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])heaps count:(NSUInteger)count
{
    (void)heaps;
    (void)count;
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

@interface RMCommandBuffer (Answers)
@end
@implementation RMCommandBuffer (Answers)

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
