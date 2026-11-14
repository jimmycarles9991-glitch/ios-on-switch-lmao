// SPDX-License-Identifier: MIT
// Rule clear.twod: a fill outside a pass (fillTexture:level:slice:region:color:) by the 2D engine's solid rectangle
// (kits/SwitchGM20BEmit/SwitchGM20BEmit2D.h, the EXA form; an sRGB target through its UNORM view with the colour encoded),
// per format, where gpu.x1.twod_clear's cells of that format read exact on the chip (RGBA8, BGRA8, their sRGB forms).
// Those cells filled plain memory only, so a target with comptags keeps the 3D engine's CLEAR_SURFACE over the region
// in a pass of its own (RenderEncoder::clearRegion), with the ZBC table's value; so does every other format.
#pragma once
#include <cmath>
#include <cstdint>

namespace raw {

constexpr bool twodFillHolds(uint32_t format, bool comptags)
{
    if (comptags)
        return false;
    switch (format) {
    case 70:  // RGBA8Unorm
        return true;
    case 71:  // RGBA8Unorm_sRGB
        return true;
    case 80:  // BGRA8Unorm
        return true;
    case 81:  // BGRA8Unorm_sRGB
        return true;
    default:
        return false;
    }
}

// The solid colour word of an 8-bit RGBA/BGRA format (its UNORM view's A8B8G8R8 / A8R8G8B8 packing), the colour channels of
// an sRGB format encoded; Metal's float-to-unorm rounding (nearest, ties away)
inline uint32_t twodFillWord(uint32_t format, const float rgba[4])
{
    const bool srgb = format == 71 || format == 81, bgra = format == 80 || format == 81;
    uint32_t code[4];
    for (int c = 0; c < 4; c++) {
        float v = std::fmin(std::fmax(rgba[c], 0.0f), 1.0f);
        if (srgb && c < 3)
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
        code[c] = uint32_t(v * 255.0f + 0.5f);
    }
    return code[3] << 24 | (bgra ? code[0] << 16 | code[1] << 8 | code[2] : code[2] << 16 | code[1] << 8 | code[0]);
}

}  // namespace raw
