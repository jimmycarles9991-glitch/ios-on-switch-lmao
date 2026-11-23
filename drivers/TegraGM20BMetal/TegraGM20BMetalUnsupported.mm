// SPDX-License-Identifier: MIT
// What the device has no form of, answered where Metal lets a caller ask: the capability queries (device_answers.h),
// the creations that may fail (nil, with an error where Metal gives one), and the calls a caller reaches only past a NO.
#import "TegraGM20BMetalInternal.h"
#include "device_answers.h"

const char *gm20bUnsupportedRenderState(MTLRenderPipelineDescriptor *d)
{
    if (d.vertexFunction.patchType != MTLPatchTypeNone &&
        d.maxTessellationFactor > gm20b::answers::thisKindCount("maxTessellationFactor"))
        return "a maximum tessellation factor past the device's maxTessellationFactor";
    // past the amplify macro's mapping words (SWITCH_GM20B_AMP_MAX), which supportsVertexAmplificationCount: answers to
    if (d.maxVertexAmplificationCount > gm20b::answers::thisKindLastYes("supportsVertexAmplificationCount:"))
        return "a vertex amplification count supportsVertexAmplificationCount: answers NO for";
    return nullptr;
}

id gm20bRefuseUnsupported(const char *why, NSError **error)
{
    SWITCH_REFUSE_DETAIL("gm20b", "state.unsupported", uintptr_t(why), "state refused at creation: %s", why);
    if (error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorUnsupported
                                 userInfo:@{
                                     NSLocalizedDescriptionKey : @(why)
                                 }];
    return nil;
}
