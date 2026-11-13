// SPDX-License-Identifier: MIT
// What a compile request carries of a render pipeline descriptor (raw_compile_request.h), as plain values: every field
// RawPipelineKey.h's pipelineKey reads, and the ones m2v's translation reads beside them (rasterizationEnabled). The client
// reads them off its descriptor and the shader compiler makes a descriptor of them (RawCompileRequest.mm), then keys it with
// pipelineKey itself, so the two sides share one key function and a field this list misses shows as a key that differs.
#pragma once
#include <cstdint>
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

struct PipelineFields {
    uint64_t samples = 1, topology = 0, depthFormat = 0, stencilFormat = 0, rasterization = 1;
    ColorFields color[8];
    AttributeFields attributes[31];
};

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
