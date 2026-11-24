// SPDX-License-Identifier: MIT
// See TegraGM20BMetalCompileRequest.h.
#import "TegraGM20BMetalCompileRequest.h"
#import "TegraGM20BMetalPipelineKey.h"

#include <cstdlib>

namespace gm20b {

MTLFunctionConstantValues *constantValuesOf(const std::string &text)
{
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find(' ', at);
        end = end == std::string::npos ? text.size() : end;
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        const size_t hexAt = item.rfind(':');
        const size_t typeAt = hexAt == std::string::npos || !hexAt ? std::string::npos : item.rfind(':', hexAt - 1);
        if (typeAt == std::string::npos || typeAt == 0)
            return nil;
        const uint64_t type = strtoull(item.c_str() + typeAt + 1, nullptr, 10);
        const std::string hex = item.substr(hexAt + 1);
        const size_t bytes = constantBytes(type);
        if (!bytes || hex.size() != 2 * bytes)
            return nil;
        uint8_t value[32] = {};
        for (size_t i = 0; i < bytes; i++)
            value[i] = uint8_t(strtoul(hex.substr(2 * i, 2).c_str(), nullptr, 16));
        const std::string name = item.substr(0, typeAt);
        if (name[0] == '#')
            [values setConstantValue:value type:MTLDataType(type) atIndex:strtoul(name.c_str() + 1, nullptr, 10)];
        else
            [values setConstantValue:value type:MTLDataType(type) withName:@(name.c_str())];
    }
    return values;
}

void applyFields(const PipelineFields &f, MTLRenderPipelineDescriptor *d)
{
    d.rasterSampleCount = f.samples;
    d.inputPrimitiveTopology = MTLPrimitiveTopologyClass(f.topology);
    d.depthAttachmentPixelFormat = MTLPixelFormat(f.depthFormat);
    d.stencilAttachmentPixelFormat = MTLPixelFormat(f.stencilFormat);
    d.rasterizationEnabled = f.rasterization != 0;
    d.maxVertexAmplificationCount = f.amplification;
    if (f.textureWriteRounding)
        [d setValue:@(f.textureWriteRounding) forKey:@"textureWriteRoundingMode"];
    d.alphaToCoverageEnabled = f.alphaToCoverage != 0;
    const TessellationFields &t = f.tessellation;
    d.tessellationPartitionMode = MTLTessellationPartitionMode(t.partition);
    d.tessellationFactorFormat = MTLTessellationFactorFormat(t.factorFormat);
    d.tessellationFactorStepFunction = MTLTessellationFactorStepFunction(t.factorStep);
    d.tessellationOutputWindingOrder = MTLWinding(t.winding);
    d.tessellationControlPointIndexType = MTLTessellationControlPointIndexType(t.controlPointIndex);
    d.maxTessellationFactor = t.maxFactor;
    d.tessellationFactorScaleEnabled = t.factorScale != 0;
    for (unsigned i = 0; i < 8; i++) {
        const ColorFields &c = f.color[i];
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        a.pixelFormat = MTLPixelFormat(c.format);
        a.writeMask = MTLColorWriteMask(c.writeMask);
        a.blendingEnabled = c.blending != 0;
        a.sourceRGBBlendFactor = MTLBlendFactor(c.sourceRGB);
        a.destinationRGBBlendFactor = MTLBlendFactor(c.destinationRGB);
        a.rgbBlendOperation = MTLBlendOperation(c.operationRGB);
        a.sourceAlphaBlendFactor = MTLBlendFactor(c.sourceAlpha);
        a.destinationAlphaBlendFactor = MTLBlendFactor(c.destinationAlpha);
        a.alphaBlendOperation = MTLBlendOperation(c.operationAlpha);
    }
    MTLVertexDescriptor *v = nil;
    for (unsigned i = 0; i < 31; i++) {
        const AttributeFields &a = f.attributes[i];
        if (!a.format)
            continue;
        if (!v)
            v = [MTLVertexDescriptor vertexDescriptor];
        v.attributes[i].format = MTLVertexFormat(a.format);
        v.attributes[i].offset = a.offset;
        v.attributes[i].bufferIndex = a.buffer;
        MTLVertexBufferLayoutDescriptor *l = v.layouts[a.buffer < 31 ? a.buffer : 0];
        l.stride = a.stride;
        l.stepFunction = MTLVertexStepFunction(a.step);
        l.stepRate = a.rate;
    }
    d.vertexDescriptor = v;
}

std::string requestPackKey(const CompileRequest &r, bool &own)
{
    const auto frameworkAt = [&](const RequestStage &s) {
        return frameworkOf(r.libraries[s.library].path);
    };
    const RequestStage &vertex = r.stages[0];
    const RequestStage *fragment = fragmentStage(r) == kNoStage ? nullptr : &r.stages[fragmentStage(r)];
    const std::string framework = frameworkAt(vertex);
    bool packable = !framework.empty() && (!fragment || framework == frameworkAt(*fragment));
    std::string linked;
    for (size_t i = firstLinked(r); i < r.stages.size(); ++i) {
        const RequestStage &s = r.stages[i];
        linked += linkedSegment(s.tag.c_str(), {s.function, s.constants}, s.name);
        packable &= !frameworkAt(s).empty();
    }
    own = !packable;
    const StageKey v{vertex.function, vertex.constants};
    const StageKey f{fragment ? fragment->function : "", fragment ? fragment->constants : ""};
    return pipelineKey(r.fields, &v, fragment ? &f : nullptr, linked, framework) + r.variant.name();
}

StageInFields stageInFieldsOf(MTLStageInputOutputDescriptor *in)
{
    StageInFields f;
    for (unsigned i = 0; in && i < 31; i++) {
        MTLAttributeDescriptor *a = in.attributes[i];
        if (a.format == MTLAttributeFormatInvalid)
            continue;
        MTLBufferLayoutDescriptor *l = in.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
        f.attributes[i] = {a.format, a.offset, a.bufferIndex, l.stride, l.stepFunction, l.stepRate};
    }
    if (in) {
        f.indexType = in.indexType;
        f.indexBuffer = in.indexBufferIndex;
    }
    return f;
}

MTLStageInputOutputDescriptor *stageInOf(const StageInFields &f)
{
    if (stageInText(f).empty())
        return nil;
    MTLStageInputOutputDescriptor *in = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
    for (unsigned i = 0; i < 31; i++) {
        const AttributeFields &a = f.attributes[i];
        if (!a.format)
            continue;
        in.attributes[i].format = MTLAttributeFormat(a.format);
        in.attributes[i].offset = a.offset;
        in.attributes[i].bufferIndex = a.buffer;
        MTLBufferLayoutDescriptor *l = in.layouts[a.buffer < 31 ? a.buffer : 0];
        l.stride = a.stride;
        l.stepFunction = MTLStepFunction(a.step);
        l.stepRate = a.rate;
    }
    in.indexType = MTLIndexType(f.indexType);
    in.indexBufferIndex = f.indexBuffer;
    return in;
}

ProbeTextures probeTexturesOf(const RenderVariant &v)
{
    ProbeTextures t;
    t.cubic = v.cubic;
    t.fragmentTests = v.fragmentTests;
    t.xrSlots = v.xrSlots;
    t.written = v.written;
    return t;
}

}  // namespace gm20b
