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
#include "../runtime/RawRenderKey.h"
#include "../runtime/raw_xr_pass.h"
#include "probe_states.h"

namespace probe {

// probe.mm's tagged textures: BGRA8, but for the raw path's own states that read codes (pack/xr.metal, packed YCbCr planes),
// the two-plane stand-in and, for its own resolves alone (pack/resolve.metal, pack/stencil_bits.metal), multisampled colours,
// depth or integers
inline raw::ProbeTextures probeTextures(const std::string &line, bool ours)
{
    if (ours && line.find("| v resolve_vert | f resolve_depth") != std::string::npos)
        return {MTLPixelFormatDepth32Float, 4};
    if (ours && line.find("| f stencil_bits_ms_") != std::string::npos)
        return {MTLPixelFormatR32Uint, 4};
    if (ours && line.find("| v resolve_vert | f resolve_") != std::string::npos)
        return {MTLPixelFormatBGRA8Unorm, 4};
    const std::pair<const char *, raw::ProbeTextures> sets[] = {
        {"f xr_bgr10_frag", {MTLPixelFormatR32Uint, 1}},       {"f xr_bgra10_frag", {MTLPixelFormatRGBA16Uint, 1}},
        {"f xr_planes_frag", {MTLPixelFormatRG32Uint, 1}},     {"f xr_two_planes_frag", {MTLPixelFormatR32Uint, 1}},
        {"f two_plane_store", {MTLPixelFormatRGBA16Float, 1}}, {"f ycbcr_store_", {MTLPixelFormatRGBA32Float, 1}},
        {"f ycbcr_packed_", {MTLPixelFormatR32Uint, 1}},       {"f turn_copy", {MTLPixelFormatR32Uint, 1}},
        {"f turn_masked", {MTLPixelFormatR32Uint, 1}},         {"f turn_reach", {MTLPixelFormatR32Uint, 1}},
        {"f turn_depth", {MTLPixelFormatDepth32Float, 1}},     {"f stencil_bits_", {MTLPixelFormatR32Uint, 1}}};
    for (const auto &[fragment, textures] : sets)
        if (line.find(std::string("| ") + fragment) != std::string::npos)
            return textures;
    return {};
}

// Whether a state's key names an RGB10A8_2P_XR10 colour ("c<j>=550:"): a state with a two-plane form (raw_xr_pass.h)
inline bool twoPlaneColors(const std::string &key)
{
    const std::string head = key.substr(0, key.find(" | "));
    for (size_t at = head.find(" c"); at != std::string::npos; at = head.find(" c", at + 2))
        if (head.compare(at + 4, 4, "550:") == 0)
            return true;
    return false;
}

// Each state of `lines` (corpusLines') once, made with its functions' library: Core Animation's `qc`, another framework's
// default.metallib or a bundle's library under PROBE_LIB_ROOT, the raw path's own `own` (`standIn` for the two-plane moves).
// `each(line, ours, state)` gets the line without its mark and the state keyed as the raw path keys it: a bundle's under its
// functions' AIR too (RawRenderKey.h, as RawMetalStates.mm looks it up).
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
        const std::string name = framework ? frameworkName(marked.substr(3, pathEnd - 3)) : "";
        if (!parseState(library, line, s, name))
            continue;
        raw::CacheKey airKeyed;
        if (framework && name.empty()) {
            const std::string cubic = s.cubic.empty() ? std::string() : s.cubic.key();
            if (!raw::renderCacheKey(s.d, s.key.substr(0, s.key.size() - cubic.size()), true, 0, airKeyed))
                continue;
            s.key = airKeyed.key + cubic;
        }
        if (seen.insert(s.key + (s.xrSlots ? raw::xrSampledVariant(s.xrSlots) : std::string()) +
                        raw::writtenVariant(s.written))
                .second)
            each(line, ours, s);
    }
}

}  // namespace probe
