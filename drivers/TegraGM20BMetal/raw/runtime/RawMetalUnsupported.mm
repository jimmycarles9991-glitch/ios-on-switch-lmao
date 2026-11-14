// SPDX-License-Identifier: MIT
// What the device has no form of, answered where Metal lets a caller ask: the capability queries (raw_device_answers.h),
// the creations that may fail (nil, with an error where Metal gives one), and the calls a caller reaches only past a NO.
#import "RawMetalInternal.h"
#include "raw_device_answers.h"

const char *rmUnsupportedRenderState(MTLRenderPipelineDescriptor *d)
{
    if (d.vertexFunction.patchType != MTLPatchTypeNone &&
        d.maxTessellationFactor > raw::answers::thisKindCount("maxTessellationFactor"))
        return "a maximum tessellation factor past the device's maxTessellationFactor";
    // past the amplify macro's mapping words (SWITCH_GM20B_AMP_MAX), which supportsVertexAmplificationCount: answers to
    if (d.maxVertexAmplificationCount > raw::answers::thisKindLastYes("supportsVertexAmplificationCount:"))
        return "a vertex amplification count supportsVertexAmplificationCount: answers NO for";
    return nullptr;
}

id rmRefuseUnsupported(const char *why, NSError **error)
{
    SWITCH_REFUSE_DETAIL("raw", "state.unsupported", uintptr_t(why), "state refused at creation: %s", why);
    if (error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorUnsupported
                                 userInfo:@{
                                     NSLocalizedDescriptionKey : @(why)
                                 }];
    return nil;
}
