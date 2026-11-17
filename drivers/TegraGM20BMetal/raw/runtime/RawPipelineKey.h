// SPDX-License-Identifier: MIT
// A render pipeline state's key: what the runtime looks a state up in the pack by (RawMetalLibrary.mm
// raw_metal_pipeline_key), what the pack generator files its record under (pack/probe_states.h) and what the shader
// compiler checks a request's key by (RawCompileRequest.mm), all made here from the descriptor's fields alone
// (raw_pipeline_fields.h) and the constant values its functions were made with, so no corpus line's text decides a pack
// key and a field a compile request does not carry cannot be in one.
// The journal's R-line fields in its order but the label (NativeBridgeMetalFrontend.mm journalRenderPipeline), each with
// a blank after it, then the stages as their library names them with their constants, the linked functions, and the
// framework where it is not QuartzCore; no trailing blank.
#pragma once
#import <Metal/Metal.h>
#include <cstdio>
#include <string>
#include "raw_pipeline_fields.h"

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

// The descriptor's fields a key names (raw_pipeline_fields.h): what a compile request carries of it, so the shader compiler
// keys a request with the same pipelineKey the client keyed its descriptor with
inline PipelineFields fieldsOf(MTLRenderPipelineDescriptor *d)
{
    PipelineFields f;
    f.samples = d.rasterSampleCount;
    f.topology = d.inputPrimitiveTopology;
    f.depthFormat = d.depthAttachmentPixelFormat;
    f.stencilFormat = d.stencilAttachmentPixelFormat;
    f.rasterization = d.rasterizationEnabled ? 1 : 0;
    f.amplification = d.maxVertexAmplificationCount;
    f.patchType = d.vertexFunction.patchType;
    f.tessellation = {d.tessellationPartitionMode,
                      d.tessellationFactorFormat,
                      d.tessellationFactorStepFunction,
                      d.tessellationOutputWindingOrder,
                      d.tessellationControlPointIndexType,
                      d.maxTessellationFactor,
                      d.tessellationFactorScaleEnabled ? 1u : 0u};
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        f.color[i] = {a.pixelFormat,
                      a.writeMask,
                      a.blendingEnabled ? 1u : 0u,
                      a.sourceRGBBlendFactor,
                      a.destinationRGBBlendFactor,
                      a.rgbBlendOperation,
                      a.sourceAlphaBlendFactor,
                      a.destinationAlphaBlendFactor,
                      a.alphaBlendOperation};
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
            f.attributes[i] = {a.format, a.offset, a.bufferIndex, l.stride, l.stepFunction, l.stepRate};
        }
    return f;
}

// `linked`: the linked functions' segments in the descriptor's order; `framework`: the vertex function's
// (<name>.framework/default.metallib), "" for another library
inline std::string pipelineKey(const PipelineFields &f, const StageKey *vertex, const StageKey *fragment,
                               const std::string &linked, const std::string &framework)
{
    char head[160];
    snprintf(head, sizeof head, "samples=%llu topo=%llu ds=%llu/%llu ", (unsigned long long)f.samples,
             (unsigned long long)f.topology, (unsigned long long)f.depthFormat, (unsigned long long)f.stencilFormat);
    std::string key = head;
    for (unsigned i = 0; i < 8; i++) {
        const ColorFields &c = f.color[i];
        if (!c.format)
            continue;
        snprintf(head, sizeof head, "c%u=%llu:%llu:%d:%llu:%llu:%llu:%llu:%llu:%llu ", i, (unsigned long long)c.format,
                 (unsigned long long)c.writeMask, int(c.blending), (unsigned long long)c.sourceRGB,
                 (unsigned long long)c.destinationRGB, (unsigned long long)c.operationRGB,
                 (unsigned long long)c.sourceAlpha, (unsigned long long)c.destinationAlpha,
                 (unsigned long long)c.operationAlpha);
        key += head;
    }
    for (unsigned i = 0; i < 31; i++) {
        const AttributeFields &a = f.attributes[i];
        if (!a.format)
            continue;
        // MTLBufferLayoutStrideDynamic as UINT32_MAX, which raw_emit.cpp emitVertexInput reads back as the bind's
        const unsigned long long stride = a.stride == UINT64_MAX ? UINT32_MAX : a.stride;
        snprintf(head, sizeof head, "a%u=%llu:%llu:%llu:%llu:%llu:%llu ", i, (unsigned long long)a.format,
                 (unsigned long long)a.offset, (unsigned long long)a.buffer, stride, (unsigned long long)a.step,
                 (unsigned long long)a.rate);
        key += head;
    }
    // a tessellation state's tessellator state, and a vertex amplification count above Metal's default: no other key has them
    if (f.patchType) {
        const TessellationFields &t = f.tessellation;
        snprintf(head, sizeof head, "t=%llu:%llu:%llu:%llu:%llu:%llu:%d ", (unsigned long long)t.partition,
                 (unsigned long long)t.factorFormat, (unsigned long long)t.factorStep, (unsigned long long)t.winding,
                 (unsigned long long)t.controlPointIndex, (unsigned long long)t.maxFactor, int(t.factorScale));
        key += head;
    }
    if (f.amplification > 1) {
        snprintf(head, sizeof head, "amp=%llu ", (unsigned long long)f.amplification);
        key += head;
    }
    // a state Metal does not rasterize: the raw path draws it with the rasteriser off (raw_stage_switches.h)
    if (!f.rasterization)
        key += "rast=0 ";
    key += " | v " + (vertex ? vertex->function : std::string("-")) + " " + (vertex ? vertex->constants : "");
    key += " | f " + (fragment ? fragment->function : std::string("-")) + " " + (fragment ? fragment->constants : "");
    key += linked;
    if (!framework.empty() && framework != "QuartzCore")
        key += " | l " + framework;
    while (!key.empty() && key.back() == ' ')
        key.pop_back();
    return key;
}

inline std::string pipelineKey(MTLRenderPipelineDescriptor *d, const StageKey *vertex, const StageKey *fragment,
                               const std::string &linked, const std::string &framework)
{
    return pipelineKey(fieldsOf(d), vertex, fragment, linked, framework);
}

}  // namespace raw
