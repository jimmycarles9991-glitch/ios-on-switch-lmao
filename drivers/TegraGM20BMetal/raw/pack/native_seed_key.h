// SPDX-License-Identifier: MIT
#pragma once
#include "../../precompile/PrecompileInputs.h"
#include "../runtime/RawPipelineKey.h"
#include "../runtime/RawRenderKey.h"

namespace rawseed {

inline std::string framework(const precompile::Stage &stage)
{
    if (stage.library.empty() || stage.library == precompile::kQuartzCore)
        return "QuartzCore";
    NSString *path = @(stage.library.c_str());
    return [path hasSuffix:@".framework/default.metallib"]
               ? path.stringByDeletingLastPathComponent.lastPathComponent.stringByDeletingPathExtension.UTF8String
               : "";
}

inline raw::StageKey functionKey(const precompile::Stage &stage)
{
    return {stage.name, raw::constantsText(precompile::constants(stage.constants))};
}

inline bool cacheKey(const precompile::RenderInput &input, uint64_t stack, raw::CacheKey &out)
{
    const auto vertex = functionKey(input.vertex), fragment = functionKey(input.fragment);
    const auto vertexFramework = framework(input.vertex);
    bool packable = !vertexFramework.empty() && vertexFramework == framework(input.fragment);
    std::string linked;
    for (const char *tag : {"vl", "vp", "fl", "fp"})
        for (const auto &[kind, source] : input.linked)
            if (kind == tag) {
                linked += raw::linkedSegment(tag, functionKey(source), source.as.empty() ? source.name : source.as);
                packable &= !framework(source).empty();
            }
    const auto key = raw::pipelineKey(input.descriptor, &vertex, &fragment, linked, vertexFramework);
    return raw::renderCacheKey(input.descriptor, key, !packable, stack, out);
}

}  // namespace rawseed
