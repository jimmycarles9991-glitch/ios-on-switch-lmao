// SPDX-License-Identifier: MIT
// Apple's extended-range formats on the raw path: RGB10A8_2P_XR10 (550), BGRA10_XR (552) and BGR10_XR (554). Each color channel
// is a 10-bit code c read as (c - 384) / 510 (Metal's MTLPixelFormat documentation). A texture holds the codes as m2v holds them
// (metal2vulkan metal/format ExtendedRange::raw_format), so the two share its memory:
//  - BGR10_XR: one word, blue at bit 0, red at bit 20, bits 30 and 31 set by a write;
//  - BGRA10_XR: four 16-bit fields, blue first, each code in the field's top 10 bits;
//  - RGB10A8_2P_XR10: a word as BGR10_XR's (bits 30 and 31 clear) and the alpha byte, side by side in 8 bytes, in a texture of a
//    descriptor; one over a two-plane surface is held in Metal's two planes (raw_xr_pass.h), and a buffer's is never one.
//    Metal's blits between such a texture and a buffer move plane 0 alone, each texel's color word (bufferTexelBytes), and
//    leave the alpha plane as it is.
// A pass renders the codes through the format's unsigned-integer target: the pack's states encode them in the function (m2v's
// encode, measured against Apple's GPUs). A draw samples a stand-in instead (Texture::standIn): an RGBA16Float image of the codes'
// values, which a pass of the pack's conversion state (pack/xr.metal) draws before the work that samples it. Core Animation's
// functions read these textures as half, and each code's value rounded to half is what Metal's nearest read returns.
// The _sRGB twins (551, 553, 555) read through Metal's own tables and are not kept here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace raw {

enum class XrForm : uint8_t {
    None,
    Planes,  // RGB10A8_2P_XR10
    Bgra10,  // BGRA10_XR
    Bgr10    // BGR10_XR
};

inline XrForm xrForm(uint32_t mtlPixelFormat)
{
    switch (mtlPixelFormat) {
    case 550:
        return XrForm::Planes;
    case 552:
        return XrForm::Bgra10;
    case 554:
        return XrForm::Bgr10;
    default:
        return XrForm::None;
    }
}

// The pack's conversion state of a form (pack/xr.metal, keyed "raw.xr.<fragment function>"), null for none; `twoPlanes`:
// RGB10A8_2P_XR10 held in its two planes (raw_xr_pass.h), read through both
inline const char *xrConversion(XrForm f, bool twoPlanes = false)
{
    switch (f) {
    case XrForm::Planes:
        return twoPlanes ? "raw.xr.xr_two_planes_frag" : "raw.xr.xr_planes_frag";
    case XrForm::Bgra10:
        return "raw.xr.xr_bgra10_frag";
    case XrForm::Bgr10:
        return "raw.xr.xr_bgr10_frag";
    case XrForm::None:
        break;
    }
    return nullptr;
}

// The bytes of each texel a blit between a texture of `mtlPixelFormat` (`bpp` bytes a texel) and a buffer moves in the buffer
inline uint32_t bufferTexelBytes(uint32_t mtlPixelFormat, uint32_t bpp)
{
    return xrForm(mtlPixelFormat) == XrForm::Planes ? 4 : bpp;
}

constexpr uint32_t kXrStandInFormat = 115;  // RGBA16Float

namespace xr_detail {

inline float toHalf(double v)
{
    return float(_Float16(float(v)));
}

// The code Metal's write stores for `v` (m2v ExtendedRange::code): a NaN is 0; color 384 +- RoundHalfEven(min(|v|, 1.5) * 510)
// of the exact product, clamped to [0, 1023]; alpha clamped to [0, 1], then 384 + RoundHalfEven(a * 510), or the two-plane byte
// RoundHalfEven(a * 255)
inline uint32_t writeCode(XrForm f, float v, bool alpha)
{
    if (std::isnan(v))
        v = 0.0f;
    if (alpha) {
        const double a = std::clamp(double(v), 0.0, 1.0);
        return f == XrForm::Planes ? uint32_t(std::nearbyint(a * 255.0)) : 384u + uint32_t(std::nearbyint(a * 510.0));
    }
    const int64_t k = int64_t(std::nearbyint(std::min(std::fabs(double(v)), 1.5) * 510.0));
    return uint32_t(std::clamp<int64_t>(v < 0.0f ? 384 - k : 384 + k, 0, 1023));
}

// The code of Metal's straight clear route for a BGR10_XR or BGRA10_XR channel (m2v ExtendedRange::straight_code):
// (v + 384/510) * 510 in float steps, ties up, clamped to [0, 1023]; a color NaN 0, alpha clamped to [0, 1] first and a NaN 384
inline uint32_t straightCode(float v, bool alpha)
{
    if (std::isnan(v)) {
        if (!alpha)
            return 0;
        v = 0.0f;
    }
    if (alpha)
        v = std::clamp(v, 0.0f, 1.0f);
    const float sum = v + 384.0f / 510.0f;
    const float x = sum * 510.0f;
    return uint32_t(std::clamp(std::floor(double(x) + 0.5), 0.0, 1023.0));
}

}  // namespace xr_detail

// The raw texel Metal's Clear load stores for `color` (red, green, blue, alpha) as the components of the form's integer target
// (m2v ExtendedRange::clear_texel). ThroughHalf: each channel through float and then half, each to nearest even, stored as a
// write stores it. `straight` (Metal's route for a texture it keeps compressed, in a pass that records no draw call; BGR10_XR and
// BGRA10_XR only): each code straight from the float, BGR10_XR's bits 30 and 31 zero.
inline void xrClearTexel(XrForm f, const float color[4], bool straight, uint32_t out[4])
{
    using namespace xr_detail;
    straight = straight && f != XrForm::Planes;
    uint32_t code[4];
    for (int c = 0; c < 4; c++)
        code[c] = straight ? straightCode(color[c], c == 3) : writeCode(f, toHalf(color[c]), c == 3);
    const uint32_t word = code[0] << 20 | code[1] << 10 | code[2];
    out[0] = out[1] = out[2] = out[3] = 0;
    switch (f) {
    case XrForm::Bgr10:
        out[0] = straight ? word : (0xC0000000u | word);
        break;
    case XrForm::Planes:
        out[0] = word;
        out[1] = code[3];
        break;
    case XrForm::Bgra10:
        out[0] = code[2] << 6;
        out[1] = code[1] << 6;
        out[2] = code[0] << 6;
        out[3] = code[3] << 6;
        break;
    case XrForm::None:
        break;
    }
}

}  // namespace raw
