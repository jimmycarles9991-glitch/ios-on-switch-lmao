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
    // an integer of `bits` bits: Metal's double truncated toward zero and saturated (a NaN 0), handed as the integer's bits
    // (integerClear: the encoder puts them in the clear's float slots, which a target of such a format passes through)
    Uint,
    Sint,
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
                       kUfloat = ClearChannel::Ufloat, kUint = ClearChannel::Uint, kSint = ClearChannel::Sint;

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
    {13, {kUint, kNone, kNone, kNone}, {8, 0, 0, 0}},                      // R8Uint
    {14, {kSint, kNone, kNone, kNone}, {8, 0, 0, 0}},                      // R8Sint
    {23, {kUint, kNone, kNone, kNone}, {16, 0, 0, 0}},                     // R16Uint
    {24, {kSint, kNone, kNone, kNone}, {16, 0, 0, 0}},                     // R16Sint
    {33, {kUint, kUint, kNone, kNone}, {8, 8, 0, 0}},                      // RG8Uint
    {34, {kSint, kSint, kNone, kNone}, {8, 8, 0, 0}},                      // RG8Sint
    {53, {kUint, kNone, kNone, kNone}, {32, 0, 0, 0}},                     // R32Uint
    {54, {kSint, kNone, kNone, kNone}, {32, 0, 0, 0}},                     // R32Sint
    {63, {kUint, kUint, kNone, kNone}, {16, 16, 0, 0}},                    // RG16Uint
    {64, {kSint, kSint, kNone, kNone}, {16, 16, 0, 0}},                    // RG16Sint
    {73, {kUint, kUint, kUint, kUint}, {8, 8, 8, 8}},                      // RGBA8Uint
    {74, {kSint, kSint, kSint, kSint}, {8, 8, 8, 8}},                      // RGBA8Sint
    {91, {kUint, kUint, kUint, kUint}, {10, 10, 10, 2}},                   // RGB10A2Uint
    {103, {kUint, kUint, kNone, kNone}, {32, 32, 0, 0}},                   // RG32Uint
    {104, {kSint, kSint, kNone, kNone}, {32, 32, 0, 0}},                   // RG32Sint
    {113, {kUint, kUint, kUint, kUint}, {16, 16, 16, 16}},                 // RGBA16Uint
    {114, {kSint, kSint, kSint, kSint}, {16, 16, 16, 16}},                 // RGBA16Sint
    {123, {kUint, kUint, kUint, kUint}, {32, 32, 32, 32}},                 // RGBA32Uint
    {124, {kSint, kSint, kSint, kSint}, {32, 32, 32, 32}},                 // RGBA32Sint
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
        case ClearChannel::Uint:
        case ClearChannel::Sint:
            out[i] = bitsOf(x);
            break;
        }
    }
    return true;
}

// A Metal clear's integer channel (m2v clear_value.rs): truncated toward zero, saturated to the channel, a NaN 0
inline uint32_t integerClear(double v, bool sign, unsigned bits)
{
    if (std::isnan(v))
        return 0;
    const double lo = sign ? -std::ldexp(1.0, int(bits) - 1) : 0.0;
    const double hi = sign ? std::ldexp(1.0, int(bits) - 1) - 1 : std::ldexp(1.0, int(bits)) - 1;
    const int64_t k = int64_t(std::clamp(std::trunc(v), lo, hi));
    return uint32_t(k);
}

// An integer format's clear as the float slots clearValue passes through (each the channel's word's bits); false for a format
// whose channels are not integers
inline bool integerClearSlots(uint32_t format, const double in[4], float out[4])
{
    const ClearFormat *f = clearFormat(format);
    if (!f || (f->kind[0] != ClearChannel::Uint && f->kind[0] != ClearChannel::Sint))
        return false;
    for (int i = 0; i < 4; i++) {
        const uint32_t word =
            f->kind[i] == ClearChannel::Unused ? 0 : integerClear(in[i], f->kind[i] == ClearChannel::Sint, f->bits[i]);
        memcpy(&out[i], &word, 4);
    }
    return true;
}

}  // namespace raw
