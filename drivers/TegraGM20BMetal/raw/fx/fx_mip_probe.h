// SPDX-License-Identifier: MIT
// The 2D engine's surface format for a chain format no probe has covered yet (twodFormat 0, a pack mip state of its own):
// what HW Test gpu.raw.mip_twod's probe cells run on the 2D engine in a debug build (switchRawProbeMipCandidates:) to compare
// with that format's 3D passes. A format joins twodFormat once its cells are within one code; 0 here: no 2D format.
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"

namespace raw {
namespace fx {

inline uint32_t twodCandidateFormat(uint32_t mtl)
{
    switch (mtl) {
    case 1:
        return NV902D_SET_DST_FORMAT_V_A8;
    case 20:
        return NV902D_SET_DST_FORMAT_V_Y16;
    case 25:
        return NV902D_SET_DST_FORMAT_V_RF16;
    case 40:
        return NV902D_SET_DST_FORMAT_V_R5G6B5;
    case 43:
        return NV902D_SET_DST_FORMAT_V_A1R5G5B5;
    case 55:
        return NV902D_SET_DST_FORMAT_V_RF32;
    case 60:
        return NV902D_SET_DST_FORMAT_V_R16_G16;
    case 65:
        return NV902D_SET_DST_FORMAT_V_RF16_GF16;
    case 90:
        return NV902D_SET_DST_FORMAT_V_A2B10G10R10;
    case 92:
        return NV902D_SET_DST_FORMAT_V_BF10GF11RF11;
    case 94:
        return NV902D_SET_DST_FORMAT_V_A2R10G10B10;
    case 105:
        return NV902D_SET_DST_FORMAT_V_RF32_GF32;
    case 110:
        return NV902D_SET_DST_FORMAT_V_R16_G16_B16_A16;
    case 125:
        return NV902D_SET_DST_FORMAT_V_RF32_GF32_BF32_AF32;
    default:
        return 0;
    }
}

}  // namespace fx
}  // namespace raw
