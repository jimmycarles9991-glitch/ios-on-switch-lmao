// SPDX-License-Identifier: MIT
#pragma once
#include "../precompile/PrecompileInputs.h"
#include "../TegraGM20BMetalPipelineKey.h"
#include "../TegraGM20BMetalRenderKey.h"

namespace gm20b_seed {

inline std::string framework(const precompile::Stage &stage)
{
    if (stage.library.empty() || stage.library == precompile::kQuartzCore)
        return "QuartzCore";
    NSString *path = @(stage.library.c_str());
    return [path hasSuffix:@".framework/default.metallib"]
               ? path.stringByDeletingLastPathComponent.lastPathComponent.stringByDeletingPathExtension.UTF8String
               : "";
}

inline gm20b::StageKey functionKey(const precompile::Stage &stage)
{
    return {stage.name, gm20b::constantsText(precompile::constants(stage.constants))};
}

inline bool cacheKey(const precompile::RenderInput &input, uint64_t stack, gm20b::CacheKey &out)
{
    const auto vertex = functionKey(input.vertex), fragment = functionKey(input.fragment);
    const auto vertexFramework = framework(input.vertex);
    bool packable = !vertexFramework.empty() && vertexFramework == framework(input.fragment);
    std::string linked;
    for (const char *tag : {"vl", "vp", "fl", "fp"})
        for (const auto &[kind, source] : input.linked)
            if (kind == tag) {
                linked += gm20b::linkedSegment(tag, functionKey(source), source.as.empty() ? source.name : source.as);
                packable &= !framework(source).empty();
            }
    const auto key = gm20b::pipelineKey(input.descriptor, &vertex, &fragment, linked, vertexFramework);
    return gm20b::renderCacheKey(input.descriptor, key, !packable, stack, out);
}

}  // namespace gm20b_seed
