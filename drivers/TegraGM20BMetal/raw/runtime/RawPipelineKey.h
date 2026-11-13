// SPDX-License-Identifier: MIT
// A render pipeline state's key: what the runtime looks a state up in the pack by (RawMetalLibrary.mm
// raw_metal_pipeline_key) and what the pack generator files its record under (pack/probe_states.h), both made here from
// the descriptor and the constant values its functions were made with, so no corpus line's text decides a pack key.
// The journal's R-line fields in its order but the label (NativeBridgeMetalFrontend.mm journalRenderPipeline), each with
// a blank after it, then the stages as their library names them with their constants, the linked functions, and the
// framework where it is not QuartzCore; no trailing blank.
#pragma once
#import <Metal/Metal.h>
#include <cstdio>
#include <string>

@protocol RMConstantValue  // MTLFunctionConstantValues' private entries (the frontend reads them the same way)
- (MTLDataType)dataType;
- (const void *)data;
- (NSString *)name;
- (NSUInteger)index;
@end
@interface MTLFunctionConstantValues (RMPrivate)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end

namespace raw {

// MTLDataType sizes, as the frontend journals constants (NativeBridgeMetalFrontend.mm constantBytes)
inline size_t constantBytes(uint64_t type)
{
    switch (type) {
    case 3:
    case 29:
    case 33:
        return 4;
    case 4:
    case 30:
    case 34:
        return 8;
    case 6:
    case 32:
    case 36:
        return 16;
    case 16:
    case 37:
    case 41:
        return 2;
    case 17:
    case 38:
    case 42:
        return 4;
    case 19:
    case 40:
    case 44:
        return 8;
    case 45:
    case 49:
    case 53:
        return 1;
    case 46:
    case 50:
    case 54:
        return 2;
    case 48:
    case 52:
    case 56:
        return 4;
    case 81:
    case 85:
        return 8;
    case 82:
    case 86:
        return 16;
    case 84:
    case 88:
        return 32;
    default:
        return 0;
    }
}

// "#<index>:<MTLDataType>:<hex bytes>" by index in its order, then "<name>:<MTLDataType>:<hex bytes>"; "" for no values
inline std::string constantsText(MTLFunctionConstantValues *values)
{
    std::string text;
    if (!values)
        return text;
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *groups[2] = {[snapshot newIndexedConstantArray], [snapshot newNamedConstantArray]};
    for (int g = 0; g < 2; g++)
        for (id<RMConstantValue> v in groups[g]) {
            const size_t n = constantBytes(v.dataType);
            char head[48];
            if (g)
                text += std::string(text.empty() ? "" : " ") + v.name.UTF8String;
            else {
                snprintf(head, sizeof head, "%s#%lu", text.empty() ? "" : " ", (unsigned long)v.index);
                text += head;
            }
            snprintf(head, sizeof head, ":%lu:", (unsigned long)v.dataType);
            text += head;
            for (size_t i = 0; i < n; i++) {
                snprintf(head, sizeof head, "%02x", ((const uint8_t *)v.data)[i]);
                text += head;
            }
        }
    return text;
}

// A stage's function: the library function it was made from and the text of the constants it was made with
struct StageKey {
    std::string function, constants;
};

// One linked function: " | <vl|vp|fl|fp> <library function>[ as=<its name>] <constants>"
inline std::string linkedSegment(const char *tag, const StageKey &f, const std::string &name)
{
    return std::string(" | ") + tag + " " + f.function + (name != f.function ? " as=" + name : "") + " " + f.constants;
}

// `linked`: the linked functions' segments in the descriptor's order; `framework`: the vertex function's
// (<name>.framework/default.metallib), "" for another library
inline std::string pipelineKey(MTLRenderPipelineDescriptor *d, const StageKey *vertex, const StageKey *fragment,
                               const std::string &linked, const std::string &framework)
{
    char head[160];
    snprintf(head, sizeof head, "samples=%lu topo=%lu ds=%lu/%lu ", (unsigned long)d.rasterSampleCount,
             (unsigned long)d.inputPrimitiveTopology, (unsigned long)d.depthAttachmentPixelFormat,
             (unsigned long)d.stencilAttachmentPixelFormat);
    std::string key = head;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid)
            continue;
        snprintf(head, sizeof head, "c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu ", i, (unsigned long)a.pixelFormat,
                 (unsigned long)a.writeMask, (int)a.blendingEnabled, (unsigned long)a.sourceRGBBlendFactor,
                 (unsigned long)a.destinationRGBBlendFactor, (unsigned long)a.rgbBlendOperation,
                 (unsigned long)a.sourceAlphaBlendFactor, (unsigned long)a.destinationAlphaBlendFactor,
                 (unsigned long)a.alphaBlendOperation);
        key += head;
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
            // MTLBufferLayoutStrideDynamic as UINT32_MAX, which raw_emit.cpp emitVertexInput reads back as the bind's
            const unsigned long stride = l.stride == MTLBufferLayoutStrideDynamic ? UINT32_MAX : l.stride;
            snprintf(head, sizeof head, "a%u=%lu:%lu:%lu:%lu:%lu:%lu ", i, (unsigned long)a.format,
                     (unsigned long)a.offset, (unsigned long)a.bufferIndex, stride, (unsigned long)l.stepFunction,
                     (unsigned long)l.stepRate);
            key += head;
        }
    // a tessellation state's tessellator state, and a vertex amplification count above Metal's default: no other key has them
    if (d.vertexFunction.patchType != MTLPatchTypeNone) {
        snprintf(head, sizeof head, "t=%lu:%lu:%lu:%lu:%lu:%lu:%d ", (unsigned long)d.tessellationPartitionMode,
                 (unsigned long)d.tessellationFactorFormat, (unsigned long)d.tessellationFactorStepFunction,
                 (unsigned long)d.tessellationOutputWindingOrder, (unsigned long)d.tessellationControlPointIndexType,
                 (unsigned long)d.maxTessellationFactor, (int)d.tessellationFactorScaleEnabled);
        key += head;
    }
    if (d.maxVertexAmplificationCount > 1) {
        snprintf(head, sizeof head, "amp=%lu ", (unsigned long)d.maxVertexAmplificationCount);
        key += head;
    }
    key += " | v " + (vertex ? vertex->function : std::string("-")) + " " + (vertex ? vertex->constants : "");
    key += " | f " + (fragment ? fragment->function : std::string("-")) + " " + (fragment ? fragment->constants : "");
    key += linked;
    if (!framework.empty() && framework != "QuartzCore")
        key += " | l " + framework;
    while (!key.empty() && key.back() == ' ')
        key.pop_back();
    return key;
}

}  // namespace raw
