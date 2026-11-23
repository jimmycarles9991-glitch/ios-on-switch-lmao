// SPDX-License-Identifier: MIT
// BGR10_XR_sRGB's (555), BGRA10_XR_sRGB's (553) and RGB10A8_2P_XR10_sRGB's (551) colour codes as Metal reads and writes them,
// through its own sRGB tables (pack/xr_srgb_decode.h, pack/xr_srgb_encode.h): what pack/xr.metal's sRGB conversions draw
// into a texture's stand-in, and what pack/xr_target.metal's store states store from a target's (format_standin.h).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "xr.h"

namespace gm20b {

inline constexpr uint16_t kBgr10XrSrgbDecode[640] = {
#include "pack/xr_srgb_decode.h"
};

inline constexpr uint16_t kBgr10XrSrgbFirst[640] = {
#include "pack/xr_srgb_encode.h"
};

// Metal's read of a colour code into a half: its decode step s / 8192, signed, the half nearest it with a tie away from
// zero (Metal's rounding of these steps). s is below 2^14, so rounding s to the half step of its binade (1, 2, 4 or 8 of
// 8192) is exact in integers.
inline float bgr10XrSrgbValue(uint32_t code)
{
    const uint32_t n = code < 384 ? 384 - code : code - 384;
    const uint32_t s = kBgr10XrSrgbDecode[std::min(n, 639u)];
    const uint32_t k = s < 2048 ? 1 : s < 4096 ? 2 : s < 8192 ? 4 : 8;
    const float v = float((s + k / 2) / k * k) / 8192.0f;
    return code < 384 ? -v : v;
}

// Metal's write code of a colour value (m2v ExtendedRange::code)
inline uint32_t bgr10XrSrgbCode(float v)
{
    if (std::isnan(v))
        v = 0.0f;
    const uint32_t i = uint32_t(std::nearbyint(std::fmin(std::fabs(v), 2.0f) * 8192.0f));
    uint32_t n = 0;
    for (uint32_t step = 512; step; step >>= 1)
        if (n + step < 640 && kBgr10XrSrgbFirst[n + step] <= i)
            n += step;
    return v < 0.0f ? 384 - std::min(n, 384u) : 384 + n;
}

// The word the store state writes for a stand-in texel `v`: bits 30 and 31 set, as Metal's write sets them
inline uint32_t bgr10XrSrgbWord(const float v[4])
{
    return 0xC0000000u | bgr10XrSrgbCode(v[0]) << 20 | bgr10XrSrgbCode(v[1]) << 10 | bgr10XrSrgbCode(v[2]);
}

// BGRA10_XR_sRGB's (553) fields pack/xr_target.metal's store state writes for a stand-in texel `v`, blue first, each code
// in its field's top 10 bits: blue, green and red through the tables above, alpha as BGRA10_XR's
inline void bgra10XrSrgbFields(const float v[4], uint16_t out[4])
{
    out[0] = uint16_t(bgr10XrSrgbCode(v[2]) << 6);
    out[1] = uint16_t(bgr10XrSrgbCode(v[1]) << 6);
    out[2] = uint16_t(bgr10XrSrgbCode(v[0]) << 6);
    out[3] = uint16_t(xr_detail::writeCode(XrForm::Bgra10, v[3], true) << 6);
}

// RGB10A8_2P_XR10_sRGB's (551) texel pack/xr_target.metal's store states write for a stand-in texel `v`: the colour word through
// the tables above, bits 30 and 31 clear (RGB10A8_2P_XR10's), and the alpha byte as RGB10A8_2P_XR10's write stores it
inline void rgb10a8XrSrgbTexel(const float v[4], uint32_t out[2])
{
    out[0] = bgr10XrSrgbCode(v[0]) << 20 | bgr10XrSrgbCode(v[1]) << 10 | bgr10XrSrgbCode(v[2]);
    out[1] = xr_detail::writeCode(XrForm::Planes, v[3], true);
}

}  // namespace gm20b
