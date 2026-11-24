// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalObjects.h"
#include "compute_lookup.h"

@interface SwitchMetalComputeState : SwitchMetalObject {
@public
    // its stage input's layouts are in its key (gm20b::stageInText), which no facts or reflection request carries
    bool _stageIn;
    gm20b::AirHash _air;
    std::string _constants;
    uint32_t _maxThreads, _threadLimit;
    bool _multipleOfWidth;
    gm20b::ComputeKernelCache<gm20b::ComputeState> _kernels;
    MTLComputePipelineDescriptor *_direct;
    SwitchMetalFunction *_function;  // the facade's, which a compile request names
    NSString *_libraryName;          // the library function it was made from, which an archive key names
    gm20b::PackComputeFacts _facts;  // the pack's answers, or the shader compiler's
    std::once_flag _writtenOnce;
    uint32_t _texturesWritten, _texturesFloat;
}
@end

// TegraGM20BMetalReflection.mm: the texture slots `s`'s kernel may store (a binding the reflection gives write access), read
// once from its reflection, the pack's else the shader compiler's; every slot where neither answers
uint32_t gm20bComputeTexturesWritten(SwitchMetalComputeState *s);
// The texture slots its kernel reads as floats (a binding of MTLDataTypeFloat components), read with those; none where
// neither answers
uint32_t gm20bComputeTexturesFloat(SwitchMetalComputeState *s);
