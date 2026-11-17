// SPDX-License-Identifier: MIT
// RG8Unorm_sRGB (31) on the raw path. GM20B's texture unit decodes no sRGB green of a two-component texel (NIL leaves
// R8G8_SRGB out of nil_formats.csv: "On Fermi+, the green component doesn't get decoding"), so a texture of it holds
// Metal's own bytes, two a texel (R8G8_UNORM's header), and a draw samples a stand-in (Texture::standIn): an
// RGBA8Unorm_sRGB image of the same red and green bytes read through a swizzle of red, green, zero, one, which decodes
// both channels as Metal's RG8Unorm_sRGB does (every one of the 65,536 texels, nearest and linear, on an M4 Max:
// m2vf-shapes2 probe/hwref/rg8srgb.swift). The copy engine widens the bytes into it before the work that samples it
// (BlitEncoder::widenSrgbPair), a 2D image at a time (raw_xr_parts.h). No render target: GM20B has no two-component
// sRGB colour target either, so a pass into one refuses at its own predicate.
#pragma once
#include <cstdint>

namespace raw {

constexpr uint32_t kRG8Srgb = 31;
constexpr uint32_t kSrgbPairStandInFormat = 71;  // RGBA8Unorm_sRGB
// MTLTextureSwizzle red, green, zero, one
constexpr uint8_t kSrgbPairSwizzle[4] = {2, 3, 0, 1};

inline bool srgbPair(uint32_t mtlPixelFormat)
{
    return mtlPixelFormat == kRG8Srgb;
}

}  // namespace raw
