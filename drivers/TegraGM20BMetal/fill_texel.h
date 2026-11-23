// SPDX-License-Identifier: MIT
// The texel fillTexture:level:slice:region:color:[pixelFormat:] (Apple SPI) writes: the colour packed by the fill's own rules,
// which are not a Clear load's (m2v fill_value.rs: EVs FRT-fil1 to fil4 on an Apple M4 under macOS 27.0.1 reproduced every
// texel and threshold of 74 formats; an M3 Max under macOS 26.7 packs alike). Each channel converts its double; every rule but
// the integers' takes the double's float first. A texel's fields lie from its lowest bit as the format's name orders them.
#pragma once
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "fill_texel_tables.h"

namespace gm20b {

enum class FillKind : uint8_t {
    Unorm,
    Snorm,
    Srgb,
    Uint,
    Sint,
    Half,
    Float,
    Ufloat,
    XrColor,
    XrAlpha,
    XrSrgb
};

struct FillField {
    uint8_t channel, at, bits;  // the colour channel (0 red .. 3 alpha), its lowest bit, its width
    FillKind kind;
};

struct FillFormat {
    uint32_t mtl;
    uint8_t bytes, fields;
    FillField field[4];
};

namespace fill_detail {

constexpr FillKind UN = FillKind::Unorm, SN = FillKind::Snorm, SR = FillKind::Srgb, UI = FillKind::Uint,
                   SI = FillKind::Sint, HF = FillKind::Half, FL = FillKind::Float, UF = FillKind::Ufloat,
                   XC = FillKind::XrColor, XA = FillKind::XrAlpha, XS = FillKind::XrSrgb;

inline constexpr FillFormat kFillFormats[] = {
    {1, 1, 1, {{3, 0, 8, UN}}},                                                         // A8Unorm
    {10, 1, 1, {{0, 0, 8, UN}}},                                                        // R8Unorm
    {11, 1, 1, {{0, 0, 8, SR}}},                                                        // R8Unorm_sRGB
    {12, 1, 1, {{0, 0, 8, SN}}},                                                        // R8Snorm
    {13, 1, 1, {{0, 0, 8, UI}}},                                                        // R8Uint
    {14, 1, 1, {{0, 0, 8, SI}}},                                                        // R8Sint
    {20, 2, 1, {{0, 0, 16, UN}}},                                                       // R16Unorm
    {22, 2, 1, {{0, 0, 16, SN}}},                                                       // R16Snorm
    {23, 2, 1, {{0, 0, 16, UI}}},                                                       // R16Uint
    {24, 2, 1, {{0, 0, 16, SI}}},                                                       // R16Sint
    {25, 2, 1, {{0, 0, 16, HF}}},                                                       // R16Float
    {30, 2, 2, {{0, 0, 8, UN}, {1, 8, 8, UN}}},                                         // RG8Unorm
    {31, 2, 2, {{0, 0, 8, SR}, {1, 8, 8, SR}}},                                         // RG8Unorm_sRGB
    {32, 2, 2, {{0, 0, 8, SN}, {1, 8, 8, SN}}},                                         // RG8Snorm
    {33, 2, 2, {{0, 0, 8, UI}, {1, 8, 8, UI}}},                                         // RG8Uint
    {34, 2, 2, {{0, 0, 8, SI}, {1, 8, 8, SI}}},                                         // RG8Sint
    {40, 2, 3, {{2, 0, 5, UN}, {1, 5, 6, UN}, {0, 11, 5, UN}}},                         // B5G6R5Unorm
    {41, 2, 4, {{3, 0, 1, UN}, {2, 1, 5, UN}, {1, 6, 5, UN}, {0, 11, 5, UN}}},          // A1BGR5Unorm
    {42, 2, 4, {{3, 0, 4, UN}, {2, 4, 4, UN}, {1, 8, 4, UN}, {0, 12, 4, UN}}},          // ABGR4Unorm
    {43, 2, 4, {{2, 0, 5, UN}, {1, 5, 5, UN}, {0, 10, 5, UN}, {3, 15, 1, UN}}},         // BGR5A1Unorm
    {53, 4, 1, {{0, 0, 32, UI}}},                                                       // R32Uint
    {54, 4, 1, {{0, 0, 32, SI}}},                                                       // R32Sint
    {55, 4, 1, {{0, 0, 32, FL}}},                                                       // R32Float
    {60, 4, 2, {{0, 0, 16, UN}, {1, 16, 16, UN}}},                                      // RG16Unorm
    {62, 4, 2, {{0, 0, 16, SN}, {1, 16, 16, SN}}},                                      // RG16Snorm
    {63, 4, 2, {{0, 0, 16, UI}, {1, 16, 16, UI}}},                                      // RG16Uint
    {64, 4, 2, {{0, 0, 16, SI}, {1, 16, 16, SI}}},                                      // RG16Sint
    {65, 4, 2, {{0, 0, 16, HF}, {1, 16, 16, HF}}},                                      // RG16Float
    {70, 4, 4, {{0, 0, 8, UN}, {1, 8, 8, UN}, {2, 16, 8, UN}, {3, 24, 8, UN}}},         // RGBA8Unorm
    {71, 4, 4, {{0, 0, 8, SR}, {1, 8, 8, SR}, {2, 16, 8, SR}, {3, 24, 8, UN}}},         // RGBA8Unorm_sRGB
    {72, 4, 4, {{0, 0, 8, SN}, {1, 8, 8, SN}, {2, 16, 8, SN}, {3, 24, 8, SN}}},         // RGBA8Snorm
    {73, 4, 4, {{0, 0, 8, UI}, {1, 8, 8, UI}, {2, 16, 8, UI}, {3, 24, 8, UI}}},         // RGBA8Uint
    {74, 4, 4, {{0, 0, 8, SI}, {1, 8, 8, SI}, {2, 16, 8, SI}, {3, 24, 8, SI}}},         // RGBA8Sint
    {80, 4, 4, {{2, 0, 8, UN}, {1, 8, 8, UN}, {0, 16, 8, UN}, {3, 24, 8, UN}}},         // BGRA8Unorm
    {81, 4, 4, {{2, 0, 8, SR}, {1, 8, 8, SR}, {0, 16, 8, SR}, {3, 24, 8, UN}}},         // BGRA8Unorm_sRGB
    {90, 4, 4, {{0, 0, 10, UN}, {1, 10, 10, UN}, {2, 20, 10, UN}, {3, 30, 2, UN}}},     // RGB10A2Unorm
    {91, 4, 4, {{0, 0, 10, UI}, {1, 10, 10, UI}, {2, 20, 10, UI}, {3, 30, 2, UI}}},     // RGB10A2Uint
    {92, 4, 3, {{0, 0, 11, UF}, {1, 11, 11, UF}, {2, 22, 10, UF}}},                     // RG11B10Float
    {94, 4, 4, {{2, 0, 10, UN}, {1, 10, 10, UN}, {0, 20, 10, UN}, {3, 30, 2, UN}}},     // BGR10A2Unorm
    {103, 8, 2, {{0, 0, 32, UI}, {1, 32, 32, UI}}},                                     // RG32Uint
    {104, 8, 2, {{0, 0, 32, SI}, {1, 32, 32, SI}}},                                     // RG32Sint
    {105, 8, 2, {{0, 0, 32, FL}, {1, 32, 32, FL}}},                                     // RG32Float
    {110, 8, 4, {{0, 0, 16, UN}, {1, 16, 16, UN}, {2, 32, 16, UN}, {3, 48, 16, UN}}},   // RGBA16Unorm
    {112, 8, 4, {{0, 0, 16, SN}, {1, 16, 16, SN}, {2, 32, 16, SN}, {3, 48, 16, SN}}},   // RGBA16Snorm
    {113, 8, 4, {{0, 0, 16, UI}, {1, 16, 16, UI}, {2, 32, 16, UI}, {3, 48, 16, UI}}},   // RGBA16Uint
    {114, 8, 4, {{0, 0, 16, SI}, {1, 16, 16, SI}, {2, 32, 16, SI}, {3, 48, 16, SI}}},   // RGBA16Sint
    {115, 8, 4, {{0, 0, 16, HF}, {1, 16, 16, HF}, {2, 32, 16, HF}, {3, 48, 16, HF}}},   // RGBA16Float
    {123, 16, 4, {{0, 0, 32, UI}, {1, 32, 32, UI}, {2, 64, 32, UI}, {3, 96, 32, UI}}},  // RGBA32Uint
    {124, 16, 4, {{0, 0, 32, SI}, {1, 32, 32, SI}, {2, 64, 32, SI}, {3, 96, 32, SI}}},  // RGBA32Sint
    {125, 16, 4, {{0, 0, 32, FL}, {1, 32, 32, FL}, {2, 64, 32, FL}, {3, 96, 32, FL}}},  // RGBA32Float
    {552, 8, 4, {{2, 6, 10, XC}, {1, 22, 10, XC}, {0, 38, 10, XC}, {3, 54, 10, XA}}},   // BGRA10_XR
    {553, 8, 4, {{2, 6, 10, XS}, {1, 22, 10, XS}, {0, 38, 10, XS}, {3, 54, 10, XA}}},   // BGRA10_XR_sRGB
    {554, 4, 3, {{2, 0, 10, XC}, {1, 10, 10, XC}, {0, 20, 10, XC}}},                    // BGR10_XR
    {555, 4, 3, {{2, 0, 10, XS}, {1, 10, 10, XS}, {0, 20, 10, XS}}},                    // BGR10_XR_sRGB
};

inline uint64_t mask(uint32_t bits)
{
    return bits >= 64 ? ~0ull : (1ull << bits) - 1;
}

inline uint32_t bitsOf(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// The double's nearest float, an overflow the infinity (a plain conversion past FLT_MAX is undefined)
inline float toFloat(double x)
{
    if (std::isfinite(x) && std::fabs(x) > double(FLT_MAX)) {
        const float sign = x > 0 ? 1.0f : -1.0f;
        return std::fabs(x) >= 3.4028235677973366e38 ? sign * INFINITY : sign * FLT_MAX;
    }
    return float(x);
}

inline bool quiet(double x)
{
    uint64_t d;
    memcpy(&d, &x, 8);
    return d & (1ull << 51);
}

// A NaN's sign and the top bits of its payload under the quiet NaN of a float of `bits` bits and `exponent` exponent bits
inline uint64_t nanBits(double x, uint32_t bits, uint32_t exponent)
{
    uint64_t d;
    memcpy(&d, &x, 8);
    const uint32_t mantissa = bits - 1 - exponent;
    return (d >> 63) << (bits - 1) | mask(exponent) << mantissa | 1ull << (mantissa - 1) |
           ((d >> (52 - mantissa)) & mask(mantissa));
}

// Clamped to [0, 1], times the largest code, rounded half up: a 4- or 8-bit channel's exact product, every other width's
// product rounded to a float
inline uint64_t unorm(double x, uint32_t bits)
{
    const float v = toFloat(x);
    if (std::isnan(v))
        return 0;
    const float c = std::clamp(v, 0.0f, 1.0f), m = float(mask(bits));
    const float rounded = c * m;
    const double product = bits == 4 || bits == 8 ? double(c) * double(m) : double(rounded);
    return uint64_t(std::floor(product + 0.5));
}

// Clamped to [-1, 1], the product with the largest code rounded to a float, then halves away from zero
inline uint64_t snorm(double x, uint32_t bits)
{
    const float v = toFloat(x);
    if (std::isnan(v))
        return 0;
    const float m = float(mask(bits - 1));
    const float rounded = std::clamp(v, -1.0f, 1.0f) * m;
    const double product = rounded;
    int64_t k = int64_t(std::floor(std::fabs(product) + 0.5));
    if (product < 0)
        k = -k;
    return uint64_t(k) & mask(bits);
}

// How many of the ascending thresholds `first` are at most the colour's float; `nan` for a NaN
inline uint64_t firstAtMost(const uint32_t *first, size_t n, double x, uint64_t nan)
{
    const float v = toFloat(x);
    if (std::isnan(v))
        return nan;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        float t;
        memcpy(&t, &first[mid], 4);
        if (t <= v)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// Truncated toward zero, saturating; a quiet NaN 0 and a signalling one the largest; a 32-bit channel stores 2^31 for a
// double strictly between 2^31 - 1 and 2^31
inline uint64_t uintCode(double x, uint32_t bits)
{
    const uint64_t max = mask(bits);
    if (std::isnan(x))
        return quiet(x) ? 0 : max;
    if (x <= 0.0)
        return 0;
    if (x >= double(max))
        return max;
    if (bits == 32 && x > 2147483647.0 && x < 2147483648.0)
        return 1ull << 31;
    return uint64_t(std::trunc(x));
}

// Truncated toward zero, saturating; a quiet NaN the smallest and a signalling one the largest
inline uint64_t sintCode(double x, uint32_t bits)
{
    const int64_t max = int64_t(mask(bits - 1)), min = -max - 1;
    int64_t v;
    if (std::isnan(x))
        v = quiet(x) ? min : max;
    else if (x >= double(max))
        v = max;
    else if (x <= double(min))
        v = min;
    else
        v = int64_t(std::trunc(x));
    return uint64_t(v) & mask(bits);
}

inline uint64_t halfCode(double x)
{
    if (std::isnan(x))
        return nanBits(x, 16, 5);
    const _Float16 h = _Float16(toFloat(x));
    uint16_t bits;
    memcpy(&bits, &h, 2);
    return bits;
}

inline uint64_t floatCode(double x)
{
    if (std::isnan(x))
        return nanBits(x, 32, 8);
    return bitsOf(toFloat(x));
}

// An unsigned small float of a 5-bit exponent and `mantissa` bits: the nearest code, ties to even; negative values 0, a
// finite value past the largest the largest, a NaN all ones; below the smallest normal first cut toward zero to 2^-37
inline uint64_t ufloatCode(double x, uint32_t mantissa)
{
    const float v = toFloat(x);
    const uint64_t infinity = 31ull << mantissa, steps = 1ull << mantissa;
    if (std::isnan(v))
        return infinity | mask(mantissa);
    if (v <= 0.0f)
        return 0;
    if (std::isinf(v))
        return infinity;
    const double d = v;
    if (d < std::ldexp(1.0, -14)) {
        const double cut = std::floor(d * std::ldexp(1.0, 37));
        return uint64_t(std::nearbyint(cut / std::ldexp(1.0, 23 - int(mantissa))));
    }
    int binade = int(bitsOf(v) >> 23 & 0xff) - 127;
    if (binade > 15)
        return infinity - 1;
    uint64_t k = uint64_t(std::nearbyint(d / std::ldexp(1.0, binade - int(mantissa))));
    if (k == 2 * steps) {
        binade++;
        k = steps;
    }
    if (binade > 15)
        return infinity - 1;
    return uint64_t(binade + 15) << mantissa | (k - steps);
}

// RGB9E5Float: NaN and negative channels 0, past 65408 65408; the exponent the largest channel's binade plus one (at least
// -15), one more where its mantissa rounds up to 512; every mantissa rounded half up
inline uint32_t rgb9e5Code(const double rgba[4])
{
    double v[3];
    for (int k = 0; k < 3; k++) {
        const float f = toFloat(rgba[k]);
        v[k] = std::isnan(f) ? 0.0 : double(std::clamp(f, 0.0f, 65408.0f));
    }
    const double max = std::max(v[0], std::max(v[1], v[2]));
    const uint32_t biased = bitsOf(float(max)) >> 23 & 0xff;
    int exponent = std::max(biased ? int(biased) - 127 : -127, -16) + 1;
    const auto halfUp = [](double x, int e) {
        return uint32_t(std::floor(x / std::ldexp(1.0, e - 9) + 0.5));
    };
    if (halfUp(max, exponent) == 512)
        exponent++;
    return halfUp(v[0], exponent) | halfUp(v[1], exponent) << 9 | halfUp(v[2], exponent) << 18 |
           uint32_t(exponent + 15) << 27;
}

// An extended-range code: the float clamped to [low, high] (NaN the low end), then floor((v + 384/510) * 510 + 1/2), each
// step a float operation
inline uint64_t xrCode(double x, float low, float high)
{
#pragma clang fp contract(off)  // each step rounded to a float: a fused multiply-add stores 0.25 as 512, not 511
    const float offset = float(384.0 / 510.0);
    float v = toFloat(x);
    v = std::isnan(v) ? low : std::clamp(v, low, high);
    const float sum = v + offset;
    const float scaled = sum * 510.0f;
    const float half = scaled + 0.5f;
    return uint64_t(std::floor(half));
}

inline uint64_t code(const FillField &f, double x)
{
    switch (f.kind) {
    case FillKind::Unorm:
        return unorm(x, f.bits);
    case FillKind::Snorm:
        return snorm(x, f.bits);
    case FillKind::Srgb:
        return firstAtMost(kFillSrgbFirst, sizeof kFillSrgbFirst / sizeof *kFillSrgbFirst, x, 0);
    case FillKind::Uint:
        return uintCode(x, f.bits);
    case FillKind::Sint:
        return sintCode(x, f.bits);
    case FillKind::Half:
        return halfCode(x);
    case FillKind::Float:
        return floatCode(x);
    case FillKind::Ufloat:
        return ufloatCode(x, f.bits - 5u);
    case FillKind::XrColor:
        return xrCode(x, float(-384.0 / 510.0), float(639.0 / 510.0));
    case FillKind::XrAlpha:
        return xrCode(x, 0.0f, 1.0f);
    case FillKind::XrSrgb:
        return firstAtMost(kFillXrSrgbFirst, sizeof kFillXrSrgbFirst / sizeof *kFillXrSrgbFirst, x, 384);
    }
    return 0;
}

}  // namespace fill_detail

// The bytes of the texel a colour fill of `rgba` packed in MTLPixelFormat `format` writes, from its lowest; 0 for a format
// whose fills do not follow the colour (compressed, depth, stencil, two-plane, YCbCr), whose texel `out` is then zeros: Metal
// writes one constant of its process there for every colour
inline uint32_t fillTexel(uint32_t format, const double rgba[4], uint8_t out[16])
{
    memset(out, 0, 16);
    if (format == 93) {  // RGB9E5Float
        const uint32_t word = fill_detail::rgb9e5Code(rgba);
        memcpy(out, &word, 4);
        return 4;
    }
    for (const FillFormat &f : fill_detail::kFillFormats) {
        if (f.mtl != format)
            continue;
        unsigned __int128 bits = 0;
        for (uint32_t k = 0; k < f.fields; k++) {
            const FillField &x = f.field[k];
            bits |= static_cast<unsigned __int128>(fill_detail::code(x, rgba[x.channel]) & fill_detail::mask(x.bits))
                    << x.at;
        }
        memcpy(out, &bits, f.bytes);
        return f.bytes;
    }
    return 0;
}

}  // namespace gm20b
