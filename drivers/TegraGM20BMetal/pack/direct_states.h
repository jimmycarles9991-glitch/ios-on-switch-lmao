// SPDX-License-Identifier: MIT
// A corpus's states as the direct compile (runtime/TegraGM20BMetalDirect.h) makes them, read as probe.mm reads them: the pack generator's
// directpack.mm and the host check test/directcheck.mm.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <set>
#include <string>
#include <vector>
#include "../TegraGM20BMetalDirect.h"
#include "../TegraGM20BMetalRenderKey.h"
#include "../xr_pass.h"
#include "probe_states.h"

namespace probe {

// probe.mm's tagged textures: BGRA8, but for the driver's own states that read codes (pack/xr.metal, packed YCbCr planes),
// the two-plane stand-in and, for its own resolves alone (pack/resolve.metal, pack/stencil_bits.metal), multisampled colours,
// depth or integers
inline gm20b::ProbeTextures probeTextures(const std::string &line, bool ours)
{
    if (ours && line.find("| v resolve_vert | f resolve_depth") != std::string::npos)
        return {MTLPixelFormatDepth32Float, 4};
    if (ours && line.find("| f stencil_bits_ms_") != std::string::npos)
        return {MTLPixelFormatR32Uint, 4};
    if (ours && line.find("| v resolve_vert | f resolve_") != std::string::npos)
        return {MTLPixelFormatBGRA8Unorm, 4};
    const std::pair<const char *, gm20b::ProbeTextures> sets[] = {
        {"f xr_bgr10_frag", {MTLPixelFormatR32Uint, 1}},
        {"f xr_bgr10_srgb_frag", {MTLPixelFormatR32Uint, 1}},
        {"f xr_bgra10_frag", {MTLPixelFormatRGBA16Uint, 1}},
        {"f xr_bgra10_srgb_frag", {MTLPixelFormatRGBA16Uint, 1}},
        {"f xr_planes_frag", {MTLPixelFormatRG32Uint, 1}},
        {"f xr_planes_srgb_frag", {MTLPixelFormatRG32Uint, 1}},
        {"f xr_two_planes_frag", {MTLPixelFormatR32Uint, 1}},
        {"f two_plane_store", {MTLPixelFormatRGBA16Float, 1}},
        {"f xr_two_planes_srgb_frag", {MTLPixelFormatR32Uint, 1}},
        {"f ycbcr_store_", {MTLPixelFormatRGBA32Float, 1}},
        {"f ycbcr_packed_", {MTLPixelFormatR32Uint, 1}},
        {"f turn_copy", {MTLPixelFormatR32Uint, 1}},
        {"f turn_masked", {MTLPixelFormatR32Uint, 1}},
        {"f turn_reach", {MTLPixelFormatR32Uint, 1}},
        {"f turn_depth", {MTLPixelFormatDepth32Float, 1}},
        {"f stencil_bits_", {MTLPixelFormatR32Uint, 1}},
        {"f fmt_store_", {MTLPixelFormatRGBA32Float, 1}},
        {"f xr_target_", {MTLPixelFormatRGBA16Float, 1}},
        {"f fmt_load_rgb9e5", {MTLPixelFormatR32Uint, 1}},
        {"f fmt_mip_", {MTLPixelFormatR32Uint, 1}}};
    for (const auto &[fragment, textures] : sets)
        if (line.find(std::string("| ") + fragment) != std::string::npos)
            return textures;
    return {};
}

// Each state of `lines` (corpusLines') once, made with its functions' library: Core Animation's `qc`, another framework's
// default.metallib or a bundle's library under PROBE_LIB_ROOT, the driver's own `own` (`standIn` for the two-plane moves).
// `each(line, ours, state)` gets the line without its mark and the state keyed as the driver keys it: a bundle's under its
// functions' AIR too (TegraGM20BMetalRenderKey.h, as TegraGM20BMetalStates.mm looks it up). `drop(line, why)` gets a line that makes no state.
template <typename Each, typename Drop>
void eachState(id<MTLDevice> dev, id<MTLLibrary> qc, id<MTLLibrary> own, id<MTLLibrary> standIn,
               const std::vector<std::string> &lines, Each each, Drop drop)
{
    std::set<std::string> seen;
    for (const std::string &marked : lines) {
        State s;
        const bool ours = marked.rfind("@O ", 0) == 0, framework = marked.rfind("@L ", 0) == 0;
        const size_t pathEnd = framework ? marked.find(' ', 3) : 0;
        const std::string line = ours ? marked.substr(3) : framework ? marked.substr(pathEnd + 1) : marked;
        const std::string path = framework ? marked.substr(3, pathEnd - 3) : "";
        id<MTLLibrary> library = !ours ? qc : line.find("| f two_plane_") != std::string::npos ? standIn : own;
        if (framework && !(library = frameworkLibrary(dev, path))) {
            drop(line, "its library " + path + " did not load from the crawl (PROBE_LIB_ROOT)");
            continue;
        }
        const std::string name = framework ? frameworkName(path) : "";
        std::string why;
        if (!parseState(library, line, s, name, &why)) {
            drop(line, why);
            continue;
        }
        s.library = library;
        s.framework = name;
        gm20b::CacheKey airKeyed;
        if (framework && name.empty()) {
            const std::string cubic = s.cubic.empty() ? std::string() : s.cubic.key();
            if (!gm20b::renderCacheKey(s.d, s.key.substr(0, s.key.size() - cubic.size()), true, 0, airKeyed)) {
                drop(line, "its functions' AIR keys no state (renderCacheKey)");
                continue;
            }
            s.key = airKeyed.key + cubic;
        }
        if (seen.insert(s.key + (s.xrSlots ? gm20b::xrSampledVariant(s.xrSlots) : std::string()) +
                        gm20b::writtenVariant(s.written))
                .second)
            each(line, ours, s);
    }
}

}  // namespace probe
