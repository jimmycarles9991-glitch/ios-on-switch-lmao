// SPDX-License-Identifier: MIT
// See RawCompileRequest.h.
#import "RawCompileRequest.h"
#import "RawPipelineKey.h"

#include <cstdlib>

namespace raw {

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

PipelineFields fieldsOf(MTLRenderPipelineDescriptor *d)
{
    PipelineFields f;
    f.samples = d.rasterSampleCount;
    f.topology = d.inputPrimitiveTopology;
    f.depthFormat = d.depthAttachmentPixelFormat;
    f.stencilFormat = d.stencilAttachmentPixelFormat;
    f.rasterization = d.rasterizationEnabled ? 1 : 0;
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

void applyFields(const PipelineFields &f, MTLRenderPipelineDescriptor *d)
{
    d.rasterSampleCount = f.samples;
    d.inputPrimitiveTopology = MTLPrimitiveTopologyClass(f.topology);
    d.depthAttachmentPixelFormat = MTLPixelFormat(f.depthFormat);
    d.stencilAttachmentPixelFormat = MTLPixelFormat(f.stencilFormat);
    d.rasterizationEnabled = f.rasterization != 0;
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
    const RequestStage &vertex = r.stages[0], &fragment = r.stages[1];
    const std::string framework = frameworkAt(vertex);
    bool packable = !framework.empty() && framework == frameworkAt(fragment);
    std::string linked;
    for (size_t i = 2; i < r.stages.size(); ++i) {
        const RequestStage &s = r.stages[i];
        linked += linkedSegment(s.tag.c_str(), {s.function, s.constants}, s.name);
        packable &= !frameworkAt(s).empty();
    }
    own = !packable;
    const StageKey v{vertex.function, vertex.constants}, f{fragment.function, fragment.constants};
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    applyFields(r.fields, d);
    std::string key = pipelineKey(d, &v, &f, linked, framework);
    if (!r.cubic.empty())
        key += r.cubic.key();
    if (r.fragmentTests)
        key += " | tests=" + std::to_string(r.fragmentTests);
    return key;
}

}  // namespace raw
