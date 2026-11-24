// SPDX-License-Identifier: MIT
// A render pipeline state's key: what the runtime looks a state up in the pack by (TegraGM20BMetalLibrary.mm
// metal_pipeline_key), what the pack generator files its record under (pack/probe_states.h) and what the shader
// compiler checks a request's key by (TegraGM20BMetalCompileRequest.mm), all made here from the descriptor's fields alone
// (pipeline_fields.h) and the constant values its functions were made with, so no corpus line's text decides a pack
// key and a field a compile request does not carry cannot be in one.
// The journal's R-line fields in its order but the label (NativeBridgeMetalFrontend.mm journalRenderPipeline), each with
// a blank after it, then the stages as their library names them with their constants, the linked functions, and the
// framework where it is not QuartzCore; no trailing blank.
#pragma once
#import <Metal/Metal.h>
#include <cstdio>
#include <string>
#include "pipeline_fields.h"

@protocol SwitchMetalConstantValue  // MTLFunctionConstantValues' private entries (the frontend reads them the same way)
- (MTLDataType)dataType;
- (const void *)data;
- (NSString *)name;
- (NSUInteger)index;
@end
@interface MTLFunctionConstantValues (SwitchMetalPrivate)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end

namespace gm20b {

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
        for (id<SwitchMetalConstantValue> v in groups[g]) {
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

// MTLRenderPipelineDescriptor's textureWriteRoundingMode (Apple SPI; 0 where Metal has none): 0 and 1 store a float
// colour output to a half-float target rounded toward zero, 2 to nearest even
inline uint64_t textureWriteRoundingOf(MTLRenderPipelineDescriptor *d)
{
    return [d respondsToSelector:NSSelectorFromString(@"textureWriteRoundingMode")]
               ? [[d valueForKey:@"textureWriteRoundingMode"] unsignedLongLongValue]
               : 0;
}

// The descriptor's fields a key names (pipeline_fields.h): what a compile request carries of it, so the shader compiler
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
    f.textureWriteRounding = textureWriteRoundingOf(d);
    f.patchType = d.vertexFunction.patchType;
    f.alphaToCoverage = d.alphaToCoverageEnabled ? 1 : 0;
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
    if (f.alphaToCoverage)
        key += "alphacov=1 ";
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
        // MTLBufferLayoutStrideDynamic as UINT32_MAX, which emit.cpp emitVertexInput reads back as the bind's
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
    if (f.textureWriteRounding == 2)
        key += "twr=2 ";
    // a state Metal does not rasterize: the driver draws it with the rasteriser off (stage_switches.h)
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

// The fast key, which the pack finds a state's records by (pack.h PackFastKey): what pipelineKey's text names, hashed as
// words with no text made. The descriptor's fields go in as pipelineKey prints them (a colour or attribute of no format,
// the tessellator without a patch, an amplification of 1 or less, a rounding other than 2, alpha-to-coverage off and
// rasterization on all left out, the dynamic stride as UINT32_MAX), each tagged so no two field lists hash alike; then
// each stage's stageHash, the linked functions' text's and the framework's. A hit is taken on the fast key alone without
// comparing the full key text: packed states cannot collide because the link refuses duplicates, and a state outside the
// pack collides with odds at most (fast-key rows) / 2^64 per creation.
inline uint64_t keyTextHash(const char *bytes, size_t length)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; i++)
        h = (h ^ uint8_t(bytes[i])) * 0x100000001b3ull;
    return h;
}

// A stage's part of the fast key (its function's: TegraGM20BMetalLibrary.mm makes it with the function)
inline uint64_t stageHash(const StageKey &k)
{
    const std::string named = k.function + '\0' + k.constants;
    return keyTextHash(named.data(), named.size());
}

constexpr uint64_t kKeyNoStage = 0x2d2d2d2d2d2d2d2dull;  // the fragment stage of a state with no fragment function

// The linked functions' text (linkedSegment's) and the framework, as the fast key takes them: 0 for none, QuartzCore's none
inline uint64_t linkedHash(const std::string &linked)
{
    return linked.empty() ? 0 : keyTextHash(linked.data(), linked.size());
}

inline uint64_t frameworkHash(const std::string &framework)
{
    return framework.empty() || framework == "QuartzCore" ? 0 : keyTextHash(framework.data(), framework.size());
}

class KeyWords {
public:
    void add(uint64_t w)
    {
        h_ = (h_ ^ w) * 0x9e3779b97f4a7c15ull;
        h_ ^= h_ >> 29;
    }

    uint64_t done() const
    {
        uint64_t h = h_ ^ (h_ >> 33);
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        return h ^ (h >> 33);
    }

private:
    uint64_t h_ = 0x243f6a8885a308d3ull;
};

inline uint64_t fastKey(const PipelineFields &f, uint64_t vertex, uint64_t fragment, uint64_t linked,
                        uint64_t framework)
{
    KeyWords k;
    for (const uint64_t w : {f.samples, f.topology, f.depthFormat, f.stencilFormat})
        k.add(w);
    for (unsigned i = 0; i < 8; i++) {
        const ColorFields &c = f.color[i];
        if (!c.format)
            continue;
        for (const uint64_t w : {uint64_t(0x100 + i), c.format, c.writeMask, c.blending, c.sourceRGB, c.destinationRGB,
                                 c.operationRGB, c.sourceAlpha, c.destinationAlpha, c.operationAlpha})
            k.add(w);
    }
    for (unsigned i = 0; i < 31; i++) {
        const AttributeFields &a = f.attributes[i];
        if (!a.format)
            continue;
        const uint64_t stride = a.stride == UINT64_MAX ? UINT32_MAX : a.stride;
        for (const uint64_t w : {uint64_t(0x200 + i), a.format, a.offset, a.buffer, stride, a.step, a.rate})
            k.add(w);
    }
    if (f.patchType) {
        const TessellationFields &t = f.tessellation;
        for (const uint64_t w : {uint64_t(0x300), t.partition, t.factorFormat, t.factorStep, t.winding,
                                 t.controlPointIndex, t.maxFactor, t.factorScale})
            k.add(w);
    }
    if (f.amplification > 1) {
        k.add(0x400);
        k.add(f.amplification);
    }
    if (f.textureWriteRounding == 2)
        k.add(0x500);
    if (!f.rasterization)
        k.add(0x600);
    if (f.alphaToCoverage)
        k.add(0x680);
    for (const uint64_t w : {uint64_t(0x700), vertex, fragment, linked, framework})
        k.add(w);
    return k.done();
}

inline uint64_t fastKey(const PipelineFields &f, const StageKey *vertex, const StageKey *fragment,
                        const std::string &linked, const std::string &framework)
{
    return fastKey(f, vertex ? stageHash(*vertex) : kKeyNoStage, fragment ? stageHash(*fragment) : kKeyNoStage,
                   linkedHash(linked), frameworkHash(framework));
}

// The fields the key names, read alone (fieldsOf reads every one, for a compile request), with the vertex function's
// MTLPatchType `patchType`
inline PipelineFields keyFieldsOf(MTLRenderPipelineDescriptor *d, uint64_t patchType)
{
    PipelineFields f;
    f.samples = d.rasterSampleCount;
    f.topology = d.inputPrimitiveTopology;
    f.depthFormat = d.depthAttachmentPixelFormat;
    f.stencilFormat = d.stencilAttachmentPixelFormat;
    f.rasterization = d.rasterizationEnabled ? 1 : 0;
    f.amplification = d.maxVertexAmplificationCount;
    f.textureWriteRounding = textureWriteRoundingOf(d);
    f.patchType = patchType;
    f.alphaToCoverage = d.alphaToCoverageEnabled ? 1 : 0;
    if (patchType)
        f.tessellation = {d.tessellationPartitionMode,
                          d.tessellationFactorFormat,
                          d.tessellationFactorStepFunction,
                          d.tessellationOutputWindingOrder,
                          d.tessellationControlPointIndexType,
                          d.maxTessellationFactor,
                          d.tessellationFactorScaleEnabled ? 1u : 0u};
    MTLRenderPipelineColorAttachmentDescriptorArray *colors = d.colorAttachments;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = colors[i];
        if (const MTLPixelFormat format = a.pixelFormat)
            f.color[i] = {format,
                          a.writeMask,
                          a.blendingEnabled ? 1u : 0u,
                          a.sourceRGBBlendFactor,
                          a.destinationRGBBlendFactor,
                          a.rgbBlendOperation,
                          a.sourceAlphaBlendFactor,
                          a.destinationAlphaBlendFactor,
                          a.alphaBlendOperation};
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor) {
        MTLVertexAttributeDescriptorArray *attributes = v.attributes;
        MTLVertexBufferLayoutDescriptorArray *layouts = v.layouts;
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = attributes[i];
            const MTLVertexFormat format = a.format;
            if (format == MTLVertexFormatInvalid)
                continue;
            const NSUInteger buffer = a.bufferIndex;
            MTLVertexBufferLayoutDescriptor *l = layouts[buffer < 31 ? buffer : 0];
            f.attributes[i] = {format, a.offset, buffer, l.stride, l.stepFunction, l.stepRate};
        }
    }
    return f;
}

}  // namespace gm20b
