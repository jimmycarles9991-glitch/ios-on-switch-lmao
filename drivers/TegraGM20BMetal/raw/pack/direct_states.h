// SPDX-License-Identifier: MIT
// A corpus's states as the direct compile (runtime/RawDirect.h) makes them, read as probe.mm reads them: the pack generator's
// directpack.mm and the host check test/directcheck.mm.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <set>
#include <string>
#include <vector>
#include "../runtime/RawDirect.h"
#include "probe_states.h"

namespace probe {

// probe.mm's tagged textures: BGRA8, but for the raw path's own states that read codes (pack/xr.metal), the two-plane stand-in
// and, for its own resolves alone (pack/resolve.metal), multisampled colours
inline raw::ProbeTextures probeTextures(const std::string &line, bool ours)
{
    if (ours && line.find("| v resolve_vert | f resolve_") != std::string::npos)
        return {MTLPixelFormatBGRA8Unorm, 4};
    const std::pair<const char *, raw::ProbeTextures> sets[] = {{"f xr_bgr10_frag", {MTLPixelFormatR32Uint, 1}},
                                                                {"f xr_bgra10_frag", {MTLPixelFormatRGBA16Uint, 1}},
                                                                {"f xr_planes_frag", {MTLPixelFormatRG32Uint, 1}},
                                                                {"f two_plane_store", {MTLPixelFormatRGBA16Float, 1}}};
    for (const auto &[fragment, textures] : sets)
        if (line.find(std::string("| ") + fragment) != std::string::npos)
            return textures;
    return {};
}

// Each state of `lines` (corpusLines') once, made with its functions' library: Core Animation's `qc`, another framework's
// default.metallib under PROBE_LIB_ROOT, the raw path's own `own` (`standIn` for the two-plane moves). `each(line, ours, state)`
// gets the line without its mark and the state keyed as the raw path keys it.
template <typename Each>
void eachState(id<MTLDevice> dev, id<MTLLibrary> qc, id<MTLLibrary> own, id<MTLLibrary> standIn,
               const std::vector<std::string> &lines, Each each)
{
    std::set<std::string> seen;
    for (const std::string &marked : lines) {
        State s;
        const bool ours = marked.rfind("@O ", 0) == 0, framework = marked.rfind("@L ", 0) == 0;
        const size_t pathEnd = framework ? marked.find(' ', 3) : 0;
        const std::string line = ours ? marked.substr(3) : framework ? marked.substr(pathEnd + 1) : marked;
        id<MTLLibrary> library = !ours ? qc : line.find("| f two_plane_") != std::string::npos ? standIn : own;
        if (framework && !(library = frameworkLibrary(dev, marked.substr(3, pathEnd - 3))))
            continue;
        if (!parseState(library, line, s, framework ? frameworkName(marked.substr(3, pathEnd - 3)) : ""))
            continue;
        if (seen.insert(s.key).second)
            each(line, ours, s);
    }
}

}  // namespace probe
