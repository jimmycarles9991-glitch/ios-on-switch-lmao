// SPDX-License-Identifier: MIT
// The blend colour's two float-bit vectors for a draw whose shader blends into an extended-range attachment and reads
// the blend colour (setBlendColorRed:green:blue:alpha:), as Metal reads BlendColor and OneMinusBlendColor: the first
// the colour clamped to the hull of the linear and BGRA10_XR_sRGB source ranges (a NaN to its low end) and alpha to
// [0, 1] (a NaN to 0), each rounded to half, nearest even, a colour channel at or past BGRA10_XR_sRGB's direct bound
// held past the hull's top; the second one minus each, h(1 - x) of the colour clamped to the linear source range and
// h(1 - h(a)) of alpha. The measurements behind each constant are m2v's extended_range/blend_color.rs's.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace raw::blend_color {

constexpr double kSourceLow = -1542.0 / 2048.0, kSourceHigh = 1281.0 / 1024.0;
constexpr double kHullLow = kSourceLow, kHullHigh = 1709.0 / 1024.0;
constexpr uint32_t kDirectBoundBits = 0x3FD59FD3;
constexpr float kPastDirectBound = 1710.0f / 1024.0f;

inline uint32_t bits(float x)
{
    uint32_t b;
    memcpy(&b, &x, sizeof b);
    return b;
}

inline double clamp(float x, double low, double high)
{
    return std::isnan(x) ? low : std::fmin(std::fmax(double(x), low), high);
}

// x rounded to half, nearest even, as a float (|x| below 2)
inline float halfEven(double x)
{
    if (x == 0.0)
        return 0.0f;
    uint64_t b;
    const double magnitude = std::fabs(x);
    memcpy(&b, &magnitude, sizeof b);
    const int exponent = int(b >> 52 & 0x7FF) - 1023;
    const double ulp = std::ldexp(1.0, (exponent > -14 ? exponent : -14) - 10);
    return float(std::nearbyint(x / ulp) * ulp);
}

}  // namespace raw::blend_color

namespace raw {

inline void blendColorWords(const float color[4], uint32_t words[8])
{
    using namespace blend_color;
    float directBound;
    memcpy(&directBound, &kDirectBoundBits, sizeof directBound);
    float h[4];
    for (int i = 0; i < 3; i++)
        h[i] = halfEven(color[i] >= directBound ? double(kPastDirectBound) : clamp(color[i], kHullLow, kHullHigh));
    h[3] = halfEven(clamp(color[3], 0.0, 1.0));
    for (int i = 0; i < 4; i++)
        words[i] = bits(h[i]);
    for (int i = 0; i < 3; i++)
        words[4 + i] = bits(halfEven(1.0 - clamp(color[i], kSourceLow, kSourceHigh)));
    words[7] = bits(halfEven(1.0 - double(h[3])));
}

}  // namespace raw
