// SPDX-License-Identifier: MIT
// Compute on the facade over the kernel model (facade_harness.h): a library of `kKernels` kernel functions, each
// in the pack for one threadgroup shape of its own (kernel i: i + 1 threads wide), so the facade names each function's AIR
// and keys its states as on the device.
#pragma once
#include "facade_harness.h"
#include "made_metallib.h"
#include "../compute_key.h"
#include "../metallib.h"
#include <string>
#include <vector>

// A function of no device's library: what another device's function is to the facade
@interface HarnessFunction : NSObject {
@public
    NSString *_name;
}
@end
@implementation HarnessFunction

- (NSString *)name
{
    return _name;
}

- (MTLFunctionType)functionType
{
    return MTLFunctionTypeKernel;
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
inline gm20b::PackKernel kernelRecord(uint32_t i, uint32_t maxThreads)
{
    gm20b::AirHash air;
    air.fill(uint8_t(kKernelSeed + i));
    const uint32_t threads[3] = {i + 1, 1, 1};
    const std::string key = gm20b::computeKey(air, "", maxThreads, false, threads, "", "", "");
    gm20b::PackKernel k = {};
    k.keyHash = gm20b::computeKeyHash(key);
    k.regionWord = -1;
    k.qmd[kThreadsWord] = (i + 1) << 16;
    k.qmd[kThreadsWord + 1] = 1 | 1 << 16;
    return k;
}

// Kernel `i`'s compute state's answers as the pack holds them (PackComputeFacts), so its creation asks no compiler
inline gm20b::PackComputeFacts kernelFacts(uint32_t i, uint32_t maxThreads)
{
    gm20b::AirHash air;
    air.fill(uint8_t(kKernelSeed + i));
    const std::string key = gm20b::computeStateKey(air, "", maxThreads, false);
    return {gm20b::Pack::fnv(key.data(), key.size()), 1024, 32, 0, 0};
}

inline uint32_t kernelOfQmd(const uint32_t *qmd)
{
    return (qmd[kThreadsWord] >> 16) - 1;
}

}  // namespace harness
