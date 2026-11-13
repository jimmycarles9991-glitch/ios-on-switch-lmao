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
    if (d.maxVertexAmplificationCount > raw::answers::thisKindLastYes("supportsVertexAmplificationCount:"))
        return "a vertex amplification count supportsVertexAmplificationCount: answers NO for";
    if (raw::answers::thisKindCount("supportsDynamicAttributeStride"))
        return nullptr;
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (NSUInteger i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format != MTLVertexFormatInvalid && a.bufferIndex < 31 &&
                v.layouts[a.bufferIndex].stride == MTLBufferLayoutStrideDynamic)
                return "a dynamic attribute stride: supportsDynamicAttributeStride answers NO";
        }
    return nullptr;
}

const char *rmUnsupportedComputeState(MTLComputePipelineDescriptor *d)
{
    if (raw::answers::thisKindCount("supportsDynamicAttributeStride"))
        return nullptr;
    if (MTLStageInputOutputDescriptor *in = d.stageInputDescriptor)
        for (NSUInteger i = 0; i < 31; i++) {
            MTLAttributeDescriptor *a = in.attributes[i];
            if (a.format != MTLAttributeFormatInvalid && a.bufferIndex < 31 &&
                in.layouts[a.bufferIndex].stride == MTLBufferLayoutStrideDynamic)
                return "a dynamic attribute stride: supportsDynamicAttributeStride answers NO";
        }
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
