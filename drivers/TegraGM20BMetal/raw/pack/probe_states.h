// SPDX-License-Identifier: MIT
// A corpus R line (the frontend journal's render pipeline state) as the descriptor it names, made with a library's functions: the
// pack generator's probe (probe.mm) and the host check of the runtime's own probe (raw/test/completecheck.mm) read lines alike.
#pragma once
#import <Metal/Metal.h>
#include <cstdlib>
#include <string>
#include <vector>

namespace probe {

inline std::vector<std::string> split(const std::string &s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size()) {
        size_t e = s.find(' ', at);
        if (e == std::string::npos)
            e = s.size();
        if (e > at)
            out.push_back(s.substr(at, e - at));
        at = e + 1;
    }
    return out;
}

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
    if (stage.empty() || stage[0] == "-")
        return nil;
    if (stage.size() == 1)
        return [qc newFunctionWithName:@(stage[0].c_str())];
    NSError *e = nil;
    return [qc newFunctionWithName:@(stage[0].c_str())
                    constantValues:constants(std::vector<std::string>(stage.begin() + 1, stage.end()))
                             error:&e];
}

struct State {
    std::string key, label;
    MTLRenderPipelineDescriptor *d;
    unsigned vertexBuffer = 99, vertexStride = 0;
};

// precompile.mm's R-line parser (the frontend's journal format), key = every descriptor field but the label.
inline bool parseState(id<MTLLibrary> qc, const std::string &line, State &s)
{
    size_t bar1 = line.find(" | v "), bar2 = line.find(" | f "), bar3 = line.find(" | ", bar2 + 5);
    if (bar1 == std::string::npos || bar2 == std::string::npos)
        return false;
    std::vector<std::string> head = split(line.substr(0, bar1));
    std::vector<std::string> v = split(line.substr(bar1 + 5, bar2 - bar1 - 5));
    std::vector<std::string> f =
        split(line.substr(bar2 + 5, (bar3 == std::string::npos ? line.size() : bar3) - bar2 - 5));
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    std::string key;
    for (const std::string &h : head) {
        if (h.rfind("label=", 0) == 0) {
            s.label = h.substr(6);
            if (s.label != "-")
                d.label = @(s.label.c_str());
        } else if (h.rfind("samples=", 0) == 0)
            d.rasterSampleCount = strtoul(h.c_str() + 8, nullptr, 10);
        else if (h.rfind("topo=", 0) == 0)
            d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)strtoul(h.c_str() + 5, nullptr, 10);
        else if (h.rfind("ds=", 0) == 0) {
            unsigned a = 0, b = 0;
            sscanf(h.c_str() + 3, "%u/%u", &a, &b);
            d.depthAttachmentPixelFormat = (MTLPixelFormat)a;
            d.stencilAttachmentPixelFormat = (MTLPixelFormat)b;
        } else if (h[0] == 'c' && isdigit(h[1])) {
            unsigned i, x[9] = {};
            if (sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6,
                       x + 7, x + 8) != 10 ||
                i >= 8)
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
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7 || i >= 31 ||
                x[2] >= 31)
                continue;
            if (!vd)
                vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0];
            vd.attributes[i].offset = x[1];
            vd.attributes[i].bufferIndex = x[2];
            vd.layouts[x[2]].stride = x[3];
            vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4];
            vd.layouts[x[2]].stepRate = x[5];
            s.vertexBuffer = x[2];
            s.vertexStride = x[3];
        } else
            continue;
        if (h.rfind("label=", 0) != 0)
            key += h + " ";
    }
    d.vertexDescriptor = vd;
    d.vertexFunction = function(qc, v);
    d.fragmentFunction = function(qc, f);
    if (!d.vertexFunction || (!d.fragmentFunction && !f.empty() && f[0] != "-"))
        return false;
    s.key = key + line.substr(bar1, (bar3 == std::string::npos ? line.size() : bar3) - bar1);
    s.d = d;
    return true;
}

}  // namespace probe
