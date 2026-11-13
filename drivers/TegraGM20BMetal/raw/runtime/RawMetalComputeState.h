// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalObjects.h"
#include "raw_compute_lookup.h"

@interface RMComputeState : RMObject {
@public
    bool _keyed;
    raw::AirHash _air;
    std::string _constants;
    uint32_t _maxThreads, _threadLimit;
    bool _multipleOfWidth;
    raw::ComputeKernelCache<raw::ComputeState> _kernels;
    MTLComputePipelineDescriptor *_direct;
    RMFunction *_function;         // the facade's, which a compile request names (nil: another device's)
    NSString *_libraryName;        // the library function it was made from, which an archive key names
    raw::PackComputeFacts _facts;  // the pack's answers (keyHash 0: none, m2v's state answers)
    id<MTLComputePipelineState> _metadata;
    id<MTLComputePipelineState> _compatibility;
    std::once_flag _twinOnce;
    const char *_m2vMissing;  // what of it m2v has no form of (rmM2vMissingKernel), null: nothing
}
@end
