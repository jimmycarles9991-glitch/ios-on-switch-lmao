// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace raw {

// The pack's state that draws a chain level of MTLPixelFormat `format` from the level above (pack/mip.metal), keyed
// "raw.mip.<fragment function>": the generateMipmapsForTexture: steps the 2D engine is not proven within one code of Metal's
// for (fx_mip_route.h; fx::twodChainFormat 0, or a step the 2D engine has not been proven at); null for a format it has none
// for
inline const char *mipState(uint32_t format)
{
    switch (format) {
    case 1:
        return "raw.mip.mip_a8_frag";
    case 20:
        return "raw.mip.mip_r16_frag";
    case 25:
        return "raw.mip.mip_r16f_frag";
    case 40:
        return "raw.mip.mip_b5g6r5_frag";
    case 43:
        return "raw.mip.mip_bgr5a1_frag";
    case 55:
        return "raw.mip.mip_r32f_frag";
    case 60:
        return "raw.mip.mip_rg16_frag";
    case 65:
        return "raw.mip.mip_rg16f_frag";
    case 71:
        return "raw.mip.mip_rgba8_srgb_frag";
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
    case 125:
        return "raw.mip.mip_rgba32f_frag";
    default:
        return nullptr;
    }
}

}  // namespace raw
