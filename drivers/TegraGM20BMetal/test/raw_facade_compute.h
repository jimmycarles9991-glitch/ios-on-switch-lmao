// SPDX-License-Identifier: MIT
// Compute on the raw facade over the kernel model (raw_facade_harness.h): a library of `kKernels` kernel functions, each
// in the pack for one threadgroup shape of its own (kernel i: i + 1 threads wide), and an m2v stand-in that makes the
// library's twin of the same metallib, so the facade names each function's AIR and keys its states as on the device.
#pragma once
#include "raw_facade_harness.h"
#include "made_metallib.h"
#include "../raw/runtime/raw_compute_key.h"
#include "../raw/runtime/raw_metallib.h"
#include <string>
#include <vector>

// A compute state as m2v reports it: every MTLComputePipelineState selector answers zero but these
@interface HarnessComputeState : HarnessM2V
@end
@implementation HarnessComputeState

- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    for (BOOL required : {YES, NO}) {
        const objc_method_description d =
            protocol_getMethodDescription(@protocol(MTLComputePipelineState), selector, required, YES);
        if (d.types)
            return [NSMethodSignature signatureWithObjCTypes:d.types];
    }
    return [super methodSignatureForSelector:selector];
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return 1024;
}

- (NSUInteger)threadExecutionWidth
{
    return 32;
}

@end

@interface HarnessFunction : NSObject {
@public
    NSString *_name;
    NSData *_metallib;
}
@end
@implementation HarnessFunction

- (NSString *)harnessName
{
    return _name;
}

- (NSData *)harnessMetallib
{
    return _metallib;
}

- (id)harnessComputeState
{
    return [HarnessComputeState new];
}

- (NSString *)name
{
    return _name;
}

- (MTLFunctionType)functionType
{
    return MTLFunctionTypeKernel;
}

@end

@interface HarnessLibrary : NSObject {
@public
    NSData *_metallib;
}
@end
@implementation HarnessLibrary

- (id)newFunctionWithName:(NSString *)name
{
    HarnessFunction *f = [HarnessFunction new];
    f->_name = name;
    f->_metallib = _metallib;
    return f;
}

@end

// m2v with libraries of what it is given
@interface HarnessComputeM2V : HarnessM2V
@end
@implementation HarnessComputeM2V

- (id)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    const void *bytes = nullptr;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map(data, &bytes, &size);
    HarnessLibrary *l = [HarnessLibrary new];
    l->_metallib = [NSData dataWithBytes:bytes length:size];
    (void)mapped;
    if (error)
        *error = nil;
    return l;
}

@end

namespace harness {

constexpr uint32_t kKernels = 8;
constexpr uint8_t kKernelSeed = 0x40;

inline std::string kernelName(uint32_t i)
{
    return "k" + std::to_string(i);
}

inline std::string kernelLibrary()
{
    std::vector<std::string> names;
    for (uint32_t i = 0; i < kKernels; i++)
        names.push_back(kernelName(i));
    return made_metallib::universal(names, kKernelSeed, 0xEE, std::vector<uint8_t>(kKernels, 2));
}

// QMDV01_07's CTA_THREAD_DIMENSION0-2 (clb1c0qmd.h, bits 592-639): words 18 (high half) and 19
constexpr uint32_t kThreadsWord = 18;

// Kernel i's pack record for a state made with `maxThreads` and no constants, dispatched i + 1 threads wide: its QMD names
// the shape, the field a launch is told apart by
inline raw::PackKernel kernelRecord(uint32_t i, uint32_t maxThreads)
{
    raw::AirHash air;
    air.fill(uint8_t(kKernelSeed + i));
    const uint32_t threads[3] = {i + 1, 1, 1};
    const std::string key = raw::computeKey(air, "", maxThreads, false, threads, "", "", "");
    raw::PackKernel k = {};
    k.keyHash = raw::computeKeyHash(key);
    k.regionWord = -1;
    k.qmd[kThreadsWord] = (i + 1) << 16;
    k.qmd[kThreadsWord + 1] = 1 | 1 << 16;
    return k;
}

inline uint32_t kernelOfQmd(const uint32_t *qmd)
{
    return (qmd[kThreadsWord] >> 16) - 1;
}

}  // namespace harness
