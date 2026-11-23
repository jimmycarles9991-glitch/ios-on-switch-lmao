// SPDX-License-Identifier: MIT
// Rule clear.twod: a fill outside a pass (fillTexture:level:slice:region:color:) by the 2D engine's solid rectangle
// (kits/SwitchGM20BEmit/SwitchGM20BEmit2D.h, the EXA form; an sRGB target through its UNORM view with the colour encoded),
// per format, where gpu.x1.twod_clear's cells of that format read exact on the chip (RGBA8, BGRA8, their sRGB forms,
// RGB10A2 and R8 on boots 19 and 21; R16 in the bench's own-format fills, BENCH3 g1). The colour is always one word in the
// target's own 2D format: a float colour format into another format hung GR on the chip (BENCH3 section 9). Those cells
// filled plain memory only, so a target with comptags, and every other format, takes the copy engine's constants
// (gm20b::BlitEncoder::fillRect).
#pragma once
#include <cstdint>

namespace gm20b {

// The 2D engine's surface format (cl902d.h SET_DST_FORMAT) a fill of MTLPixelFormat `format` writes and packs its colour
// word in; 0 where rule clear.twod does not hold for it
constexpr uint32_t twodFillFormat(uint32_t format)
{
    switch (format) {
    case 10:          // R8Unorm
        return 0xf3;  // Y8
    case 20:          // R16Unorm
        return 0xee;  // Y16
    case 70:          // RGBA8Unorm
    case 71:          // RGBA8Unorm_sRGB
        return 0xd5;  // A8B8G8R8
    case 80:          // BGRA8Unorm
    case 81:          // BGRA8Unorm_sRGB
        return 0xcf;  // A8R8G8B8
    case 90:          // RGB10A2Unorm
        return 0xd1;  // A2B10G10R10
    default:
        return 0;
    }
}

constexpr bool twodFillHolds(uint32_t format, bool comptags)
{
    return !comptags && twodFillFormat(format) != 0;
}

}  // namespace gm20b
