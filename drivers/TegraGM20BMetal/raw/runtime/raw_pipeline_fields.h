// SPDX-License-Identifier: MIT
// What a compile request carries of a render pipeline descriptor (raw_compile_request.h), as plain values: every field
// RawPipelineKey.h's pipelineKey reads (the vertex function's patch type among them), and the ones m2v's translation
// reads beside them (rasterizationEnabled, the tessellator's state, the amplification count). The client reads them off
// its descriptor and the shader compiler makes a descriptor of them (RawCompileRequest.mm), then keys it with
// pipelineKey itself, so the two sides share one key function and a field this list misses shows as a key that differs.
// Each struct here goes over the wire whole (raw_compile_request.cpp), so a field added to one is carried.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace raw {

struct ColorFields {
    uint64_t format = 0, writeMask = 0, blending = 0, sourceRGB = 0, destinationRGB = 0, operationRGB = 0,
             sourceAlpha = 0, destinationAlpha = 0, operationAlpha = 0;
};

// An attribute with the layout of the buffer it reads
struct AttributeFields {
    uint64_t format = 0, offset = 0, buffer = 0, stride = 0, step = 0, rate = 0;
};

// MTLRenderPipelineDescriptor's tessellation properties, Metal's defaults as defaults
struct TessellationFields {
    uint64_t partition = 0, factorFormat = 0, factorStep = 0, winding = 0, controlPointIndex = 0, maxFactor = 16,
             factorScale = 0;
};

struct PipelineFields {
    uint64_t samples = 1, topology = 0, depthFormat = 0, stencilFormat = 0, rasterization = 1;
    uint64_t amplification = 1;  // maxVertexAmplificationCount
    uint64_t patchType = 0;      // the vertex function's MTLPatchType
    TessellationFields tessellation;
    ColorFields color[8];
    AttributeFields attributes[31];
};

// A compute descriptor's stageInputDescriptor
struct StageInFields {
    AttributeFields attributes[31];
    uint64_t indexType = 0, indexBuffer = 0;
};

// The stage input's part of a compute state's key (RawMetalComputeState.mm), after its constants: " | in a<i>=<format>:
// <offset>:<buffer>:<stride>:<step>:<rate>... i=<indexType>:<indexBuffer>", the dynamic stride as UINT32_MAX; "" for none
inline std::string stageInText(const StageInFields &in)
{
    std::string text;
    char b[96];
    for (unsigned i = 0; i < 31; i++) {
        const AttributeFields &a = in.attributes[i];
        if (!a.format)
            continue;
        const unsigned long long stride = a.stride == UINT64_MAX ? UINT32_MAX : a.stride;
        snprintf(b, sizeof b, " a%u=%llu:%llu:%llu:%llu:%llu:%llu", i, (unsigned long long)a.format,
                 (unsigned long long)a.offset, (unsigned long long)a.buffer, stride, (unsigned long long)a.step,
                 (unsigned long long)a.rate);
        text += b;
    }
    if (text.empty())
        return text;
    snprintf(b, sizeof b, " i=%llu:%llu", (unsigned long long)in.indexType, (unsigned long long)in.indexBuffer);
    return " | in" + text + b;
}

// The framework a library at `path` is the default library of ("RenderBox" for .../RenderBox.framework/default.metallib),
// "" for any other library: RawMetalLibrary.mm's rule for RMLibrary's _framework
inline std::string frameworkOf(const std::string &path)
{
    static const std::string suffix = ".framework/default.metallib";
    if (path.size() <= suffix.size() || path.compare(path.size() - suffix.size(), suffix.size(), suffix))
        return std::string();
    const size_t end = path.size() - suffix.size(), slash = path.rfind('/', end - 1);
    const size_t from = slash == std::string::npos ? 0 : slash + 1;
    return path.substr(from, end - from);
}

// A library on the system volume (or an OS cryptex, which shows under /System too): the compiler reads it itself, so a
// request names it by its path alone. No relative, repeated or empty component.
inline bool systemLibraryPath(const std::string &path)
{
    if (path.rfind("/System/", 0) != 0 || path.size() > 1024 || path.find("//") != std::string::npos ||
        path.find("/./") != std::string::npos || path.find("/../") != std::string::npos)
        return false;
    static const std::string suffix = ".metallib";
    return path.size() > suffix.size() && !path.compare(path.size() - suffix.size(), suffix.size(), suffix);
}

}  // namespace raw
