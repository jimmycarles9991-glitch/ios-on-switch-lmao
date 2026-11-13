#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../NativeBridgeMetalFrontend.h"
#include "../bridge/NativeMetalDirect.h"
#include "device_compile.h"
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

struct Case {
    const char *name;
    NSString *directory;
    unsigned shape, mode;
    bool called = false;
};

static bool compile(const NmbDirectKernel *kernel, void *context)
{
    auto &test = *static_cast<Case *>(context);
    test.called = true;
    const auto compiler = raw::nvk::DeviceNvk::find(native_bridge_metal_nvk_symbol);
    raw::SubgroupSize subgroup;
    subgroup.size = kernel->stage.subgroupSize;
    raw::NvkStageInfo info = {kernel->stage.spirv,
                              kernel->stage.spirvBytes,
                              kernel->stage.entry,
                              kernel->stage.stage,
                              kernel->stage.specialization,
                              raw::handle(kernel->layout),
                              0,
                              nullptr,
                              subgroup.size ? &subgroup : nullptr,
                              false,
                              false,
                              0,
                              0,
                              {},
                              0,
                              kernel->stage.semantic,
                              kernel->stage.portableContext,
                              kernel->stage.portable};
    NSMutableDictionary *meta =
        [@{@"regionWord" : @(kernel->regionWord),
           @"pushMask" : @(kernel->pushMask)} mutableCopy];
    NSMutableArray *push = [NSMutableArray new];
    for (uint32_t word : kernel->push)
        [push addObject:@(word)];
    meta[@"push"] = push;
    for (const char *route : {"native", "portable"}) {
        if (route[0] == 'p')
            info.semantic = nullptr;
        raw::NvkStage stage = {};
        fprintf(stderr, "compile %s_%u_%u %s\n", test.name, test.shape, test.mode, route);
        int result = raw::nvk::compileStage(compiler, raw::handle(kernel->device), info, stage);
        if (result || (route[0] == 'n' && stage.semanticVersion != 1)) {
            fprintf(stderr, "compile failed: %d native=%u\n", result, stage.semanticVersion);
            return false;
        }
        NSString *name = [NSString stringWithFormat:@"%s_%u_%u.%s.sass", test.name, test.shape, test.mode, route];
        [[NSData dataWithBytes:stage.code
                        length:stage.codeBytes] writeToFile:[test.directory stringByAppendingPathComponent:name]
                                                 atomically:YES];
        raw::nvk::stageFinish(compiler, raw::handle(kernel->device), stage);
    }
    NSString *name = [NSString stringWithFormat:@"%s_%u_%u.json", test.name, test.shape, test.mode];
    [[NSJSONSerialization dataWithJSONObject:meta options:NSJSONWritingSortedKeys
                                       error:nil] writeToFile:[test.directory stringByAppendingPathComponent:name]
                                                   atomically:YES];
    return kernel->regionWord >= 0;
}

int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0, nullptr);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!library) {
            fprintf(stderr, "library: %s\n", error.description.UTF8String);
            return 1;
        }
        unsigned failures = 0;
        const uint32_t shapes[][3] = {
            {8, 5, 3}, {3, 1, 1}, {8, 1, 3}, {3, 5, 1}, {3, 5, 3}, {8, 5, 1}, {3, 1, 3}, {8, 1, 1},
        };
        for (const char *name : {"semantic_edges", "semantic_edges_barrier"}) {
            for (unsigned shape = 0; shape < std::size(shapes); ++shape) {
                for (unsigned mode = 0; mode < 2; ++mode) {
                    auto *descriptor = [MTLComputePipelineDescriptor new];
                    descriptor.computeFunction = [library newFunctionWithName:@(name)];
                    NmbDirectDispatch dispatch = {};
                    std::copy(shapes[shape], shapes[shape] + 3, dispatch.threads);
                    dispatch.regionMode = mode;
                    dispatch.memory[0] = 4 * 8 * 5 * 3;
                    Case test = {name, @(argv[4]), shape, mode};
                    const char *why = nullptr;
                    bool made = native_bridge_metal_compute_direct(descriptor, &dispatch, compile, &test, &why);
                    if (!made || !test.called) {
                        fprintf(stderr, "%s shape=%u mode=%u: %s\n", name, shape, mode, why ?: "failed");
                        ++failures;
                    }
                }
            }
        }
        return failures ? 1 : 0;
    }
}
