// SPDX-License-Identifier: MIT
// Apple's extended-range formats on the driver: RGB10A8_2P_XR10 (550), BGRA10_XR (552) and BGR10_XR (554). Each color channel
// is a 10-bit code c read as (c - 384) / 510 (Metal's MTLPixelFormat documentation). A texture holds the codes as m2v holds them
// (metal2vulkan metal/format ExtendedRange::raw_format), so the two share its memory:
//  - BGR10_XR: one word, blue at bit 0, red at bit 20, bits 30 and 31 set by a write;
//  - BGRA10_XR: four 16-bit fields, blue first, each code in the field's top 10 bits;
//  - RGB10A8_2P_XR10: a word as BGR10_XR's (bits 30 and 31 clear) and the alpha byte, side by side in 8 bytes, in a texture of a
//    descriptor; one over a two-plane surface is held in Metal's two planes (xr_pass.h), and a buffer's is never one.
//    Metal's blits between such a texture and a buffer move plane 0 alone, each texel's color word (bufferTexelBytes), and
//    leave the alpha plane as it is.
// A pass renders the codes through the format's unsigned-integer target: the pack's states encode them in the function (m2v's
// encode, measured against Apple's GPUs). A draw samples a stand-in instead (Texture::standIn): an RGBA16Float image of the codes'
// values, which a pass of the pack's conversion state (pack/xr.metal) draws before the work that samples it. Core Animation's
// functions read these textures as half, and each code's value rounded to half is what Metal's nearest read returns.
// BGR10_XR_sRGB (555) and BGRA10_XR_sRGB (553) are BGR10_XR's word and BGRA10_XR's fields with each colour code read and
// written through Metal's own sRGB tables (xr_srgb.h), BGRA10_XR_sRGB's alpha as BGRA10_XR's: their stand-ins hold those
// reads, and a pass renders them through their format's stand-in (format_standin.h), never their codes.
// RGB10A8_2P_XR10_sRGB (551) is RGB10A8_2P_XR10's planes, blits and CPU copies (twoPlaneXr), each colour code read and written
// through those sRGB tables and its alpha byte linear (m2v ExtendedRange::Planes { srgb }). xrForm answers None for it: a pass
// renders it through its format stand-in, never in its planes as RGB10A8_2P_XR10 (xr_pass.h); a draw samples its stand-in
// (xrSampled, xrConversionOf). One of a descriptor keeps its texels as RGB10A8_2P_XR10's does (an M4 Max, one level: uploads,
// CPU copies, passes and kernel writes all read back; m2v-textures3 probes/keeps.mm).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gm20b {

enum class XrForm : uint8_t {
    None,
    Planes,     // RGB10A8_2P_XR10
    Bgra10,     // BGRA10_XR
    Bgr10,      // BGR10_XR
    Bgr10Srgb,  // BGR10_XR_sRGB
    Bgra10Srgb  // BGRA10_XR_sRGB
};

inline XrForm xrForm(uint32_t mtlPixelFormat)
{
    switch (mtlPixelFormat) {
    case 550:
        return XrForm::Planes;
    case 552:
        return XrForm::Bgra10;
    case 553:
        return XrForm::Bgra10Srgb;
    case 554:
        return XrForm::Bgr10;
    case 555:
        return XrForm::Bgr10Srgb;
    default:
        return XrForm::None;
    }
}

constexpr uint32_t kRgb10A8XrSrgb = 551;

// RGB10A8_2P_XR10 or its sRGB twin: a texel of a colour word and an alpha byte, Metal's two planes
inline bool twoPlaneXr(uint32_t format)
{
    return format == 550 || format == kRgb10A8XrSrgb;
}

// Whether a draw samples a texture of `format` through an extended-range stand-in (Texture::standIn)
inline bool xrSampled(uint32_t format)
{
    return xrForm(format) != XrForm::None || format == kRgb10A8XrSrgb;
}

// The pack's conversion state of a form (pack/xr.metal, keyed "raw.xr.<fragment function>"), null for none; `twoPlanes`:
// RGB10A8_2P_XR10 held in its two planes (xr_pass.h), read through both
inline const char *xrConversion(XrForm f, bool twoPlanes = false)
{
    switch (f) {
    case XrForm::Planes:
        return twoPlanes ? "raw.xr.xr_two_planes_frag" : "raw.xr.xr_planes_frag";
    case XrForm::Bgra10:
        return "raw.xr.xr_bgra10_frag";
    case XrForm::Bgr10:
        return "raw.xr.xr_bgr10_frag";
    case XrForm::Bgr10Srgb:
        return "raw.xr.xr_bgr10_srgb_frag";
    case XrForm::Bgra10Srgb:
        return "raw.xr.xr_bgra10_srgb_frag";
    case XrForm::None:
        break;
    }
    return nullptr;
}

// The conversion state a draw samples a texture of `format` through (its stand-in's)
inline const char *xrConversionOf(uint32_t format, bool twoPlanes = false)
{
    if (format == kRgb10A8XrSrgb)
        return twoPlanes ? "raw.xr.xr_two_planes_srgb_frag" : "raw.xr.xr_planes_srgb_frag";
    return xrConversion(xrForm(format), twoPlanes);
}

// The bytes of each texel a blit between a texture of `mtlPixelFormat` (`bpp` bytes a texel) and a buffer moves in the buffer
inline uint32_t bufferTexelBytes(uint32_t mtlPixelFormat, uint32_t bpp)
{
    return twoPlaneXr(mtlPixelFormat) ? 4 : bpp;
}

constexpr uint32_t kXrStandInFormat = 115;  // RGBA16Float

// Apple's extended-range formats, each a colour attachment format: RGB10A8_2P_XR10 (550), BGRA10_XR (552), BGR10_XR (554)
// and their sRGB twins
constexpr bool xrTarget(uint32_t format)
{
    return format >= 550 && format <= 555;
}

// An extended-range attachment's pass through its RGBA16Float stand-in, Metal's tile (format_standin.h, measured on an M4
// Max by xr-float's probe): Metal's tile holds a pass in halves and stores codes once; every blend read clamps the
// destination as the source (colour codes past 1022 read as 1022, BGRA10_XR's alpha codes to 384..894); a pass rewrites
// every texel of the 32x32 tiles its fragments reach and keeps the rest. pack/xr_target.metal's load state fills the
// stand-in with the blend read of each code and its store state writes Metal's write code back; GM20B keeps no record of
// which tiles a pass's fragments reached, so a loaded pass's store keeps each texel whose stand-in still holds the load's
// value of its codes
enum class XrStep : uint8_t {
    Load,
    Store,
    Resolve
};

// The pack's state of `step` for an attachment of `format` at `samples` samples; `planes`: held in a surface's two planes
// (Texture::xrPlanes). Null where the pack has none: the two-plane formats take no sample count above 1 (Metal's rule)
constexpr const char *xrTargetState(uint32_t format, XrStep step, uint32_t samples = 1, bool planes = false)
{
    const char *const oneSample[][3] = {
        // load, store and resolve of BGR10_XR, BGRA10_XR, RGB10A8_2P_XR10 of a descriptor and over two planes; their sRGB
        // twins'
        {"raw.xr_target.xr_target_load_bgr10_frag", "raw.xr_target.xr_target_store_bgr10_frag",
         "raw.xr_target.xr_target_resolve_bgr10_frag"},
        {"raw.xr_target.xr_target_load_bgra10_frag", "raw.xr_target.xr_target_store_bgra10_frag",
         "raw.xr_target.xr_target_resolve_bgra10_frag"},
        {"raw.xr_target.xr_target_load_planes_frag", "raw.xr_target.xr_target_store_planes_frag", nullptr},
        {"raw.xr_target.xr_target_load_two_planes_frag", "raw.xr_target.xr_target_store_two_planes_frag", nullptr},
        {"raw.xr_target.xr_target_load_bgr10_srgb_frag", "raw.xr_target.xr_target_store_bgr10_srgb_frag",
         "raw.xr_target.xr_target_resolve_bgr10_srgb_frag"},
        {"raw.xr_target.xr_target_load_bgra10_srgb_frag", "raw.xr_target.xr_target_store_bgra10_srgb_frag",
         "raw.xr_target.xr_target_resolve_bgra10_srgb_frag"},
        {"raw.xr_target.xr_target_load_planes_srgb_frag", "raw.xr_target.xr_target_store_planes_srgb_frag", nullptr},
        {"raw.xr_target.xr_target_load_two_planes_srgb_frag", "raw.xr_target.xr_target_store_two_planes_srgb_frag",
         nullptr},
    };
    const char *const perSample[][2][2] = {
        // load and store at 2 and at 4 samples, of BGR10_XR, BGRA10_XR and their sRGB twins
        {{"raw.xr_target.xr_target_load_bgr10_ms2_frag", "raw.xr_target.xr_target_store_bgr10_ms2_frag"},
         {"raw.xr_target.xr_target_load_bgr10_ms4_frag", "raw.xr_target.xr_target_store_bgr10_ms4_frag"}},
        {{"raw.xr_target.xr_target_load_bgra10_ms2_frag", "raw.xr_target.xr_target_store_bgra10_ms2_frag"},
         {"raw.xr_target.xr_target_load_bgra10_ms4_frag", "raw.xr_target.xr_target_store_bgra10_ms4_frag"}},
        {{"raw.xr_target.xr_target_load_bgr10_srgb_ms2_frag", "raw.xr_target.xr_target_store_bgr10_srgb_ms2_frag"},
         {"raw.xr_target.xr_target_load_bgr10_srgb_ms4_frag", "raw.xr_target.xr_target_store_bgr10_srgb_ms4_frag"}},
        {{"raw.xr_target.xr_target_load_bgra10_srgb_ms2_frag", "raw.xr_target.xr_target_store_bgra10_srgb_ms2_frag"},
         {"raw.xr_target.xr_target_load_bgra10_srgb_ms4_frag", "raw.xr_target.xr_target_store_bgra10_srgb_ms4_frag"}},
    };
    if (!xrTarget(format))
        return nullptr;
    const bool srgb = format & 1;
    const uint32_t form = format <= 551 ? (planes ? 3 : 2) : format <= 553 ? 1 : 0;
    if (samples <= 1)
        return oneSample[form + (srgb ? 4 : 0)][uint32_t(step)];
    if (form > 1 || step == XrStep::Resolve || (samples != 2 && samples != 4))
        return nullptr;
    return perSample[form + (srgb ? 2 : 0)][samples == 4][uint32_t(step)];
}

// An RGB10A8_2P_XR10 or RGB10A8_2P_XR10_sRGB texel buffer as an M4 Max lays one in its buffer (m2v-textures3
// probes/xrbuf.mm): each texel's colour word in order, then each alpha byte in order from the words' bytes rounded up to
// 16 KiB. Its own header reads the words (R32Uint); a draw or kernel samples its stand-in.
constexpr uint32_t kXrTexelBufferWordBytes = 4, kXrTexelBufferWordFormat = 53;
// A texel buffer's header type (fx::kTexOneDBuffer, clb097tex.h TEXTURE_TYPE ONE_D_BUFFER), for the headers that see no fx
constexpr uint8_t kTexelBufferType = 6;

constexpr uint64_t texelBufferAlphaOffset(uint64_t elements)
{
    return (elements * kXrTexelBufferWordBytes + 16383) & ~uint64_t(16383);
}

// The bytes a texel buffer of `elements` texels of `format` (`bpp` bytes a texel) spans in its buffer
constexpr uint64_t texelBufferBytes(uint32_t format, uint64_t elements, uint32_t bpp)
{
    return twoPlaneXr(format) ? texelBufferAlphaOffset(elements) + elements : elements * bpp;
}

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
    case XrForm::Bgr10Srgb:  // cleared in their stand-ins (formatStandInClear), never through the clear registers
    case XrForm::Bgra10Srgb:
    case XrForm::None:
        break;
    }
}

}  // namespace gm20b
