// SPDX-License-Identifier: MIT
// Rule clear.twod: a fill outside a pass (fillTexture:level:slice:region:color:) by the 2D engine's solid rectangle
// (kits/SwitchGM20BEmit/SwitchGM20BEmit2D.h, the EXA form; an sRGB target through its UNORM view with the colour encoded),
// per format, where gpu.x1.twod_clear's cells of that format read exact on the chip (RGBA8, BGRA8, their sRGB forms,
// RGB10A2 and R8 on boots 19 and 21; R16 in the bench's own-format fills, BENCH3 g1). The colour is always one word in the
// target's own 2D format: a float colour format into another format hung GR on the chip (BENCH3 section 9). Those cells
// filled plain memory only, so a target with comptags keeps the 3D engine's CLEAR_SURFACE over the region in a pass of its
// own (RenderEncoder::clearRegion), with the ZBC table's value; so does every other format.
#pragma once
#include <cmath>
#include <cstdint>

namespace raw {

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

// The solid colour word of `format` in twodFillFormat's packing, the colour channels of an sRGB format encoded; Metal's
// float-to-unorm rounding (nearest, ties away)
inline uint32_t twodFillWord(uint32_t format, const float rgba[4])
{
    const bool srgb = format == 71 || format == 81;
    auto code = [&](int c, uint32_t max) {
        float v = std::fmin(std::fmax(rgba[c], 0.0f), 1.0f);
        if (srgb && c < 3)
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
        return uint32_t(v * float(max) + 0.5f);
    };
    switch (twodFillFormat(format)) {
    case 0xf3:
        return code(0, 255);
    case 0xee:
        return code(0, 65535);
    case 0xd5:
        return code(3, 255) << 24 | code(2, 255) << 16 | code(1, 255) << 8 | code(0, 255);
    case 0xcf:
        return code(3, 255) << 24 | code(0, 255) << 16 | code(1, 255) << 8 | code(2, 255);
    case 0xd1:
        return code(3, 3) << 30 | code(2, 1023) << 20 | code(1, 1023) << 10 | code(0, 1023);
    default:
        return 0;
    }
}

}  // namespace raw
