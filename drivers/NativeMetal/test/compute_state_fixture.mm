// SPDX-License-Identifier: MIT
#import "../../TegraGM20BMetal/raw/runtime/RawMetalComputeState.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#if SWITCH_FRAME_TIMELINE
std::atomic<uint64_t> gTwinsMade;
thread_local uint64_t tTwinTicks;

void countTwin(id, uint64_t)
{}
#endif

@implementation RMObject

- (id)rmTwin
{
    return _twin;
}

- (Class)rmTwinClass
{
    return Nil;
}

- (id)device
{
    return _device;
}

@end
@implementation RMFunction

- (MTLFunctionType)functionType
{
    return [(id<MTLFunction>)_twin functionType];
}

@end

void once(NSString *text)
{
    fprintf(stderr, "%s\n", text.UTF8String);
}

@interface ArtifactFallback : NSObject {
@public
    std::atomic<unsigned> calls;
    bool fail;
}
@property (nonatomic) id<MTLDevice> backend;
@end

@implementation ArtifactFallback

- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor
                                    options:(MTLPipelineOption)options
                                 reflection:(MTLAutoreleasedComputePipelineReflection *)reflection
                                      error:(NSError **)error
{
    calls.fetch_add(1, std::memory_order_relaxed);
    if (fail) {
        if (error)
            *error = [NSError errorWithDomain:@"compute-state-test" code:1 userInfo:nil];
        return nil;
    }
    return [_backend newComputePipelineStateWithDescriptor:descriptor
                                                   options:options
                                                reflection:reflection
                                                     error:error];
}

@end

unsigned computeStateChecks(id<MTLDevice> native, MTLComputePipelineDescriptor *descriptor)
{
    unsigned failures = 0;
    const auto check = [&](bool good, const char *name) {
        printf("%s %s\n", good ? "PASS" : "FAIL", name);
        failures += !good;
    };
    ArtifactFallback *fallback = [ArtifactFallback new];
    fallback.backend = native;
    RMFunction *function = [RMFunction new];
    function->_twin = descriptor.computeFunction;
    function.name = descriptor.computeFunction.name;
    MTLComputePipelineDescriptor *wrapped = [descriptor copy];
    wrapped.computeFunction = (id<MTLFunction>)function;
    NSError *error = nil;
    RMComputeState *state = rmComputeState(native, wrapped, MTLPipelineOptionNone, nil, &error);
    check(state && state->_keyed && !error && !state->_compatibility && fallback->calls.load() == 0,
          "facade creates no compatibility pipeline");
    if (!state)
        return failures + 1;
    auto pso = (id<MTLComputePipelineState>)state;
    check(pso.maxTotalThreadsPerThreadgroup == 128 && pso.threadExecutionWidth == 32 &&
              pso.staticThreadgroupMemoryLength == 128 && pso.device == native && fallback->calls.load() == 0,
          "facade metadata does not create a twin");
    check([state conformsToProtocol:@protocol(MTLComputePipelineState)] &&
              [state respondsToSelector:@selector(maxTotalThreadsPerThreadgroup)] && fallback->calls.load() == 0,
          "facade introspection does not create a twin");
    std::array<id, 16> values;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < values.size(); ++i)
        workers.emplace_back([&, i] {
            @autoreleasepool {
                for (unsigned n = 0; n < 64; ++n) {
                    [state respondsToSelector:@selector(staticThreadgroupMemoryLength)];
                    [state conformsToProtocol:@protocol(MTLComputePipelineState)];
                    [state rmTwinClass];
                }
                values[i] = [state rmTwin];
            }
        });
    for (auto &worker : workers)
        worker.join();
    check(values[0] && fallback->calls.load() == 1 &&
              std::all_of(values.begin(), values.end(),
                          [&](id value) {
                              return value == values[0];
                          }),
          "concurrent compatibility requests publish one pipeline");
    check(pso.maxTotalThreadsPerThreadgroup == 128 && pso.threadExecutionWidth == 32,
          "native metadata survives compatibility publication");

    fallback->fail = true;
    state = rmComputeState(native, wrapped, MTLPipelineOptionNone, nil, &error);
    const unsigned before = fallback->calls.load();
    check(state && ![state rmTwin] && ![state rmTwin] && fallback->calls.load() == before + 1,
          "failed deferred creation stays a failed compatibility request");
    state = rmComputeState(native, descriptor, MTLPipelineOptionNone, nil, &error);
    check(!state && error && fallback->calls.load() == before + 2,
          "ineligible factory preserves immediate compatibility error");
    return failures;
}
