// SPDX-License-Identifier: MIT
// The clear value m2v hands Vulkan for a Metal clear color of an attachment (metal2vulkan-runtime clear_value.rs `color`), for
// the formats the raw path renders: what NVK's clear writes, as Metal's conversion stores it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace raw {

enum class ClearChannel : uint8_t {
    Unused,
    Unorm,      // through float, clamped, the nearest of `bits` (ties to even), handed as k / max
    HalfUnorm,  // as Unorm, of the float's nearest half
    Srgb,       // the 8-bit sRGB code of the float, handed as the linear value that encodes to it exactly
    Half,       // the float's nearest half
    Float,
    Ufloat,  // an unsigned small float of `bits` mantissa bits, truncated
};

struct ClearFormat {
    uint32_t mtl;
    ClearChannel kind[4];
    uint8_t bits[4];
};

namespace clear_detail {

inline uint32_t bitsOf(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

inline uint32_t quietNan(float x)
{
    return bitsOf(x) | 0x7fc00000u;
}

inline float unorm(float x, unsigned bits)
{
    const double max = double((1u << bits) - 1);
    const double v = std::isnan(x) ? 0.0 : std::clamp(double(x), 0.0, 1.0);
    return float(std::nearbyint(v * max) / max);
}

inline float half(float x)
{
    return float(_Float16(x));
}

inline float srgb(float x)
{
    const double v = std::isnan(x) ? 0.0 : std::clamp(double(x), 0.0, 1.0);
    const double s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    const double k = std::clamp(std::nearbyint(s * 255.0), 0.0, 255.0);
    const double e = k / 255.0;
    return float(e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4));
}

inline float ufloat(float v, unsigned mantissa)
{
    if (std::isnan(v))
        return NAN;
    if (v <= 0.0f)
        return 0.0f;
    if (std::isinf(v))
        return INFINITY;
    const double steps = double(1u << mantissa), largest = (2.0 - 1.0 / steps) * 32768.0;
    if (double(v) >= largest)
        return float(largest);
    const double spacing = std::ldexp(1.0, std::max(std::ilogb(v), -14)) / steps;
    return float(std::floor(double(v) / spacing) * spacing);
}

}  // namespace clear_detail

constexpr ClearChannel kNone = ClearChannel::Unused, kUnorm = ClearChannel::Unorm, kHalfUnorm = ClearChannel::HalfUnorm,
                       kSrgb = ClearChannel::Srgb, kHalf = ClearChannel::Half, kFloat = ClearChannel::Float,
                       kUfloat = ClearChannel::Ufloat;

inline constexpr ClearFormat kClearFormats[] = {
    {1, {kNone, kNone, kNone, kUnorm}, {0, 0, 0, 8}},                      // A8Unorm
    {10, {kUnorm, kNone, kNone, kNone}, {8, 0, 0, 0}},                     // R8Unorm
    {20, {kUnorm, kNone, kNone, kNone}, {16, 0, 0, 0}},                    // R16Unorm
    {25, {kHalf, kNone, kNone, kNone}, {0, 0, 0, 0}},                      // R16Float
    {30, {kUnorm, kUnorm, kNone, kNone}, {8, 8, 0, 0}},                    // RG8Unorm
    {40, {kHalfUnorm, kHalfUnorm, kHalfUnorm, kNone}, {5, 6, 5, 0}},       // B5G6R5Unorm
    {43, {kHalfUnorm, kHalfUnorm, kHalfUnorm, kHalfUnorm}, {5, 5, 5, 1}},  // BGR5A1Unorm
    {55, {kFloat, kNone, kNone, kNone}, {0, 0, 0, 0}},                     // R32Float
    {60, {kUnorm, kUnorm, kNone, kNone}, {16, 16, 0, 0}},                  // RG16Unorm
    {65, {kHalf, kHalf, kNone, kNone}, {0, 0, 0, 0}},                      // RG16Float
    {70, {kUnorm, kUnorm, kUnorm, kUnorm}, {8, 8, 8, 8}},                  // RGBA8Unorm
    {71, {kSrgb, kSrgb, kSrgb, kUnorm}, {8, 8, 8, 8}},                     // RGBA8Unorm_sRGB
    {80, {kUnorm, kUnorm, kUnorm, kUnorm}, {8, 8, 8, 8}},                  // BGRA8Unorm
    {81, {kSrgb, kSrgb, kSrgb, kUnorm}, {8, 8, 8, 8}},                     // BGRA8Unorm_sRGB
    {90, {kUnorm, kUnorm, kUnorm, kUnorm}, {10, 10, 10, 2}},               // RGB10A2Unorm
    {92, {kUfloat, kUfloat, kUfloat, kNone}, {6, 6, 5, 0}},                // RG11B10Float
    {94, {kUnorm, kUnorm, kUnorm, kUnorm}, {10, 10, 10, 2}},               // BGR10A2Unorm
    {105, {kFloat, kFloat, kNone, kNone}, {0, 0, 0, 0}},                   // RG32Float
    {110, {kUnorm, kUnorm, kUnorm, kUnorm}, {16, 16, 16, 16}},             // RGBA16Unorm
    {115, {kHalf, kHalf, kHalf, kHalf}, {0, 0, 0, 0}},                     // RGBA16Float
    {125, {kFloat, kFloat, kFloat, kFloat}, {0, 0, 0, 0}},                 // RGBA32Float
};

inline const ClearFormat *clearFormat(uint32_t mtl)
{
    for (const ClearFormat &f : kClearFormats)
        if (f.mtl == mtl)
            return &f;
    return nullptr;
}

// False for a format with no row above (the raw path renders none such). A NaN half or float channel is the quiet NaN of its
// bits, as m2v's value is; m2v then writes Metal's NaN texel through an integer view (nan_clear.rs), which the raw path does
// not.
inline bool clearValue(uint32_t format, const float in[4], uint32_t out[4])
{
    using namespace clear_detail;
    const ClearFormat *f = clearFormat(format);
    if (!f)
        return false;
    for (int i = 0; i < 4; i++) {
        const float x = in[i];
        switch (f->kind[i]) {
        case ClearChannel::Unused:
            out[i] = 0;
            break;
        case ClearChannel::Unorm:
            out[i] = bitsOf(unorm(x, f->bits[i]));
            break;
        case ClearChannel::HalfUnorm:
            out[i] = bitsOf(unorm(half(x), f->bits[i]));
            break;
        case ClearChannel::Srgb:
            out[i] = bitsOf(srgb(x));
            break;
        case ClearChannel::Half:
            out[i] = std::isnan(x) ? quietNan(x) : bitsOf(half(x));
            break;
        case ClearChannel::Float:
            out[i] = std::isnan(x) ? quietNan(x) : bitsOf(x);
            break;
        case ClearChannel::Ufloat:
            out[i] = bitsOf(ufloat(x, f->bits[i]));
            break;
        }
    }
    return true;
}

}  // namespace raw
