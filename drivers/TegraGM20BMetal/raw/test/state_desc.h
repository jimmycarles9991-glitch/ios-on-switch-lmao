// SPDX-License-Identifier: MIT
// A corpus R line's render pipeline descriptor over a library (Core Animation's default.metallib): its attachments, vertex
// layout and functions with their constants, as precompile.mm records them.
#pragma once
#import <Metal/Metal.h>
#include <string>
#include <vector>
#include "frame.h"

namespace state_desc {

inline MTLFunctionConstantValues *constants(const std::vector<std::string> &items)
{
    MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
    for (const std::string &item : items) {
        size_t a = item.rfind(':'), b = a == std::string::npos ? a : item.rfind(':', a - 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        std::string name = item.substr(0, b), type = item.substr(b + 1, a - b - 1), hex = item.substr(a + 1);
        uint8_t bytes[64] = {};
        for (size_t i = 0; i + 1 < hex.size() && i / 2 < sizeof bytes; i += 2)
            bytes[i / 2] = (uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
        if (name[0] == '#')
            [v setConstantValue:bytes
                           type:(MTLDataType)atoi(type.c_str())
                        atIndex:strtoul(name.c_str() + 1, nullptr, 10)];
        else
            [v setConstantValue:bytes type:(MTLDataType)atoi(type.c_str()) withName:@(name.c_str())];
    }
    return v;
}

inline id<MTLFunction> function(id<MTLLibrary> qc, const std::vector<std::string> &stage)
{
    if (stage.empty())
        return nil;
    if (stage.size() == 1)
        return [qc newFunctionWithName:@(stage[0].c_str())];
    NSError *e = nil;
    return [qc newFunctionWithName:@(stage[0].c_str())
                    constantValues:constants(std::vector<std::string>(stage.begin() + 1, stage.end()))
                             error:&e];
}

inline MTLRenderPipelineDescriptor *descriptor(id<MTLLibrary> qc, const std::string &line)
{
    size_t bar1 = line.find(" | v "), bar2 = line.find(" | f "), bar3 = line.find(" | ", bar2 + 5);
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    for (const std::string &h : frame::split(line.substr(0, bar1))) {
        if (h.rfind("label=", 0) == 0) {
            if (h != "label=-")
                d.label = @(h.substr(6).c_str());
        } else if (h.rfind("samples=", 0) == 0)
            d.rasterSampleCount = strtoul(h.c_str() + 8, nullptr, 10);
        else if (h.rfind("topo=", 0) == 0)
            d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)strtoul(h.c_str() + 5, nullptr, 10);
        else if (h.rfind("ds=", 0) == 0) {
            unsigned depth = 0, stencil = 0;
            sscanf(h.c_str() + 3, "%u/%u", &depth, &stencil);
            d.depthAttachmentPixelFormat = MTLPixelFormat(depth);
            d.stencilAttachmentPixelFormat = MTLPixelFormat(stencil);
        } else if (h[0] == 'c' && isdigit(h[1])) {
            unsigned i, x[9] = {};
            if (sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6,
                       x + 7, x + 8) != 10)
                continue;
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
            c.pixelFormat = (MTLPixelFormat)x[0];
            c.writeMask = x[1];
            c.blendingEnabled = x[2];
            c.sourceRGBBlendFactor = (MTLBlendFactor)x[3];
            c.destinationRGBBlendFactor = (MTLBlendFactor)x[4];
            c.rgbBlendOperation = (MTLBlendOperation)x[5];
            c.sourceAlphaBlendFactor = (MTLBlendFactor)x[6];
            c.destinationAlphaBlendFactor = (MTLBlendFactor)x[7];
            c.alphaBlendOperation = (MTLBlendOperation)x[8];
        } else if (h[0] == 'a' && isdigit(h[1])) {
            unsigned i, x[6] = {};
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7)
                continue;
            if (!vd)
                vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0];
            vd.attributes[i].offset = x[1];
            vd.attributes[i].bufferIndex = x[2];
            vd.layouts[x[2]].stride = x[3];
            vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4];
            vd.layouts[x[2]].stepRate = x[5];
        }
    }
    d.vertexDescriptor = vd;
    d.vertexFunction = function(qc, frame::split(line.substr(bar1 + 5, bar2 - bar1 - 5)));
    d.fragmentFunction =
        function(qc, frame::split(line.substr(bar2 + 5, (bar3 == std::string::npos ? line.size() : bar3) - bar2 - 5)));
    return d;
}

}  // namespace state_desc
