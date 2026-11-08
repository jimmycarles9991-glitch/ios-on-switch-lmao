// SPDX-License-Identifier: MIT
// Host check: the frontend's vertexAttributes and stageInputAttributes of every function of a library are this Mac's Metal's
// answers for the same library: nil or not, and each attribute's name, index, type, activity and patch flags, in order.
// usage: vertex_attributes_host <host Vulkan driver> <cache dir> <metallib>
// build: xcrun clang++ -std=c++17 -fobjc-arc -I drivers/NativeMetal drivers/NativeMetal/bridge/vertex_attributes_host.mm \
//        drivers/NativeMetal/NativeBridgeMetalFrontend.mm <libnative_metal_bridge.a> -framework Foundation -framework Metal \
//        -framework IOSurface -framework IOKit -framework CoreVideo
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../NativeBridgeMetalFrontend.h"

static NSString *describe(NSArray *attributes)
{
    if (!attributes)
        return @"nil";
    NSMutableArray *parts = [NSMutableArray array];
    for (MTLAttribute *a in attributes)
        [parts addObject:[NSString stringWithFormat:@"%@@%lu:%lu%s%s%s", a.name, (unsigned long)a.attributeIndex,
                                                    (unsigned long)a.attributeType, a.isActive ? "" : " inactive",
                                                    a.isPatchData ? " patch" : "",
                                                    a.isPatchControlPointData ? " control-point" : ""]];
    return [parts componentsJoinedByString:@", "];
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4)
            return 2;
        id<MTLDevice> ours = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        id<MTLDevice> metal = MTLCreateSystemDefaultDevice();
        NSURL *url = [NSURL fileURLWithPath:@(argv[3])];
        NSError *error = nil;
        id<MTLLibrary> ourLibrary = [ours newLibraryWithURL:url error:&error],
                       metalLibrary = [metal newLibraryWithURL:url error:&error];
        if (!ourLibrary || !metalLibrary) {
            std::printf("FAIL library: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        unsigned functions = 0, attributes = 0, failed = 0;
        for (NSString *name in metalLibrary.functionNames) {
            id<MTLFunction> expected = [metalLibrary newFunctionWithName:name],
                            got = [ourLibrary newFunctionWithName:name];
            if (!got) {
                std::printf("FAIL %s: no function\n", name.UTF8String);
                failed++;
                continue;
            }
            functions++;

            const struct {
                const char *what;
                NSArray *expected, *got;
            } pairs[] = {{"vertexAttributes", expected.vertexAttributes, got.vertexAttributes},
                         {"stageInputAttributes", expected.stageInputAttributes, got.stageInputAttributes}};

            for (const auto &p : pairs) {
                attributes += p.expected.count;
                NSString *want = describe(p.expected), *have = describe(p.got);
                if (![want isEqualToString:have]) {
                    std::printf("FAIL %s %s: Metal [%s], ours [%s]\n", name.UTF8String, p.what, want.UTF8String,
                                have.UTF8String);
                    failed++;
                }
            }
        }
        std::printf("%u functions, %u attributes, %u differ\n", functions, attributes, failed);
        return failed ? 1 : 0;
    }
}
