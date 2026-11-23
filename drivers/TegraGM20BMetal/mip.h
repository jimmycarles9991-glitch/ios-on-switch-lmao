// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace gm20b {

// The pack's state that draws a chain level of MTLPixelFormat `format` from the level above (pack/mip.metal), keyed
// "raw.mip.<fragment function>": the generateMipmapsForTexture: steps the 2D engine is not proven within one code of Metal's
// at (fx_mip_route.h; fx::twodChainFormat 0, or a step whose weights are not quarters), each texel Metal's taps; null for a
// format it has none for
inline const char *mipState(uint32_t format)
{
    switch (format) {
    case 1:
        return "raw.mip.mip_a8_frag";
    case 10:
        return "raw.mip.mip_r8_frag";
    case 12:
        return "raw.mip.mip_r8snorm_frag";
    case 20:
        return "raw.mip.mip_r16_frag";
    case 22:
        return "raw.mip.mip_r16snorm_frag";
    case 25:
        return "raw.mip.mip_r16f_frag";
    case 30:
        return "raw.mip.mip_rg8_frag";
    case 32:
        return "raw.mip.mip_rg8snorm_frag";
    case 40:
        return "raw.mip.mip_b5g6r5_frag";
    case 43:
        return "raw.mip.mip_bgr5a1_frag";
    case 55:
        return "raw.mip.mip_r32f_frag";
    case 60:
        return "raw.mip.mip_rg16_frag";
    case 62:
        return "raw.mip.mip_rg16snorm_frag";
    case 65:
        return "raw.mip.mip_rg16f_frag";
    case 70:
        return "raw.mip.mip_rgba8_frag";
    case 71:
        return "raw.mip.mip_rgba8_srgb_frag";
    case 72:
        return "raw.mip.mip_rgba8snorm_frag";
    case 80:
        return "raw.mip.mip_bgra8_frag";
    case 81:
        return "raw.mip.mip_bgra8_srgb_frag";
    case 90:
        return "raw.mip.mip_rgb10a2_frag";
    case 92:
        return "raw.mip.mip_rg11b10f_frag";
    case 94:
        return "raw.mip.mip_bgr10a2_frag";
    case 105:
        return "raw.mip.mip_rg32f_frag";
    case 110:
        return "raw.mip.mip_rgba16_frag";
    case 112:
        return "raw.mip.mip_rgba16snorm_frag";
    case 115:
        return "raw.mip.mip_rgba16f_frag";
    case 125:
        return "raw.mip.mip_rgba32f_frag";
    default:
        return nullptr;
    }
}

// The pack's state that draws a plane of a 3D texture's level from the level above (pack/mip.metal mip_3d_*): one for every
// format mipState names a state for, and those the 2D engine filters, keyed "raw.mip.mip_3d_<format>_frag"; null for none
inline const char *mip3dState(uint32_t format)
{
    switch (format) {
    case 1:
        return "raw.mip.mip_3d_a8_frag";
    case 10:
        return "raw.mip.mip_3d_r8_frag";
    case 12:
        return "raw.mip.mip_3d_r8snorm_frag";
    case 20:
        return "raw.mip.mip_3d_r16_frag";
    case 22:
        return "raw.mip.mip_3d_r16snorm_frag";
    case 25:
        return "raw.mip.mip_3d_r16f_frag";
    case 30:
        return "raw.mip.mip_3d_rg8_frag";
    case 32:
        return "raw.mip.mip_3d_rg8snorm_frag";
    case 40:
        return "raw.mip.mip_3d_b5g6r5_frag";
    case 43:
        return "raw.mip.mip_3d_bgr5a1_frag";
    case 55:
        return "raw.mip.mip_3d_r32f_frag";
    case 60:
        return "raw.mip.mip_3d_rg16_frag";
    case 62:
        return "raw.mip.mip_3d_rg16snorm_frag";
    case 65:
        return "raw.mip.mip_3d_rg16f_frag";
    case 70:
        return "raw.mip.mip_3d_rgba8_frag";
    case 71:
        return "raw.mip.mip_3d_rgba8_srgb_frag";
    case 72:
        return "raw.mip.mip_3d_rgba8snorm_frag";
    case 80:
        return "raw.mip.mip_3d_bgra8_frag";
    case 81:
        return "raw.mip.mip_3d_bgra8_srgb_frag";
    case 90:
        return "raw.mip.mip_3d_rgb10a2_frag";
    case 92:
        return "raw.mip.mip_3d_rg11b10f_frag";
    case 94:
        return "raw.mip.mip_3d_bgr10a2_frag";
    case 105:
        return "raw.mip.mip_3d_rg32f_frag";
    case 110:
        return "raw.mip.mip_3d_rgba16_frag";
    case 112:
        return "raw.mip.mip_3d_rgba16snorm_frag";
    case 115:
        return "raw.mip.mip_3d_rgba16f_frag";
    case 125:
        return "raw.mip.mip_3d_rgba32f_frag";
    default:
        return nullptr;
    }
}

// The pack's states that draw a chain level of a format whose words the GM20B renders but cannot filter (pack/fmt.metal
// fmt_mip_*): the extended-range formats (xr.h) and their sRGB twins, and in 3D the formats the GM20B renders none of
// (format_standin.h), whose 2D steps formatMipPasses draws. Each reads the level above's words as the integers of
// wordMipView and sums its taps' codes in integers, at an M4 Max's taps, weights and grid for the format, word for word
// what its generateMipmapsForTexture: writes under macOS 27.2.1 (harness/miporacle.mm). RGB10A8_2P_XR10 and its sRGB twin
// have none: an M4 Max keeps no texel of theirs outside a surface's two planes, so their rule is unmeasured.
constexpr const char *wordMipState(uint32_t format)
{
    switch (format) {
    case 552:
        return "raw.fmt.fmt_mip_bgra10_xr_frag";
    case 553:
        return "raw.fmt.fmt_mip_bgra10_xr_srgb_frag";
    case 554:
        return "raw.fmt.fmt_mip_bgr10_xr_frag";
    case 555:
        return "raw.fmt.fmt_mip_bgr10_xr_srgb_frag";
    default:
        return nullptr;
    }
}

// The same of a 3D chain's plane, from the two planes of the level above that Metal's step reads
constexpr const char *wordMip3dState(uint32_t format)
{
    switch (format) {
    case 11:
        return "raw.fmt.fmt_mip_3d_srgb8_frag";
    case 41:
        return "raw.fmt.fmt_mip_3d_a1bgr5_frag";
    case 42:
        return "raw.fmt.fmt_mip_3d_abgr4_frag";
    case 93:
        return "raw.fmt.fmt_mip_3d_rgb9e5_frag";
    case 552:
        return "raw.fmt.fmt_mip_3d_bgra10_xr_frag";
    case 553:
        return "raw.fmt.fmt_mip_3d_bgra10_xr_srgb_frag";
    case 554:
        return "raw.fmt.fmt_mip_3d_bgr10_xr_frag";
    case 555:
        return "raw.fmt.fmt_mip_3d_bgr10_xr_srgb_frag";
    default:
        return nullptr;
    }
}

// The integer format a word mip state reads and writes a level's words as
constexpr uint32_t wordMipView(uint32_t format)
{
    switch (format) {
    case 11:
        return 13;  // R8Uint
    case 41:
    case 42:
        return 23;  // R16Uint
    case 552:
    case 553:
        return 113;  // RGBA16Uint
    default:
        return 53;  // R32Uint
    }
}

struct Texture;
class CommandBuffer;
class Device;

// Levels first + 1 .. last of the selected layers (a 3D chain: every plane) of chain `t` from the level above by its
// format's word mip state, one pass a level, layer and plane; false: *why
bool wordMipPasses(Device &d, CommandBuffer &rc, Texture &t, uint32_t first, uint32_t last, uint32_t firstLayer,
                   uint32_t layers, uint32_t format, const char **why);

}  // namespace gm20b
