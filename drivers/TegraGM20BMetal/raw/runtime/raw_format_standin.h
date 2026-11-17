// SPDX-License-Identifier: MIT
// Colour targets of the formats the GM20B renders none of (FormatInfo rt 0) that Metal renders: R8Unorm_sRGB (11),
// A1BGR5Unorm (41), ABGR4Unorm (42) and RGB9E5Float (93). A pass renders such an attachment into a block-linear stand-in of
// the command buffer's (backing tag kFormatStandInTag + colour), its states made for the stand-in's format
// (RawMetalYcbcrTargets.mm standInTargetState), filled from the texture before a pass that does not clear it and put back
// after one that stores it. As an M4 Max renders them (probe/fmt: fmtq, edges):
//  - R8Unorm_sRGB holds 8-bit sRGB codes after every draw: an RGBA8Unorm_sRGB stand-in, whose first byte the copy engine
//    moves in and out (BlitEncoder::copyFirstByte);
//  - A1BGR5Unorm and ABGR4Unorm hold a pass in halves and store once: a drawn channel's code is rint(clamp(half(v)) * max), a
//    NaN 0, a clear's floor(clamp(v) * max + 0.5). An RGBA32Float stand-in, which the pack's load state fills with the
//    texels' floats and whose floats its store state turns into the codes of a drawn channel, into the texture's words viewed
//    as R16Uint; a clear goes into it as its codes' floats. Blends land within one code: Metal blends halves of its own
//    rounding, the stand-in floats;
//  - RGB9E5Float holds its own words after every draw, each mantissa truncated, a clear's rounded: the same stand-in, the store
//    state truncating, into the words viewed as R32Uint. A pass whose draws do not blend over one another stores Metal's words;
//    blends over earlier draws lie within the truncations Metal makes between them.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace raw {

constexpr uint32_t kFormatStandInTag = 112;

constexpr uint32_t kR8UnormSrgb = 11, kA1Bgr5Unorm = 41, kAbgr4Unorm = 42, kRgb9e5Float = 93;

// The format a pass renders such an attachment into, 0 for any other
constexpr uint32_t formatStandIn(uint32_t format)
{
    switch (format) {
    case kR8UnormSrgb:
        return 71;  // RGBA8Unorm_sRGB
    case kA1Bgr5Unorm:
    case kAbgr4Unorm:
    case kRgb9e5Float:
        return 125;  // RGBA32Float
    default:
        return 0;
    }
}

// Whether the format holds alpha: one that does not reads a destination alpha of 1 (raw_standin_blend.h)
constexpr bool formatHoldsAlpha(uint32_t format)
{
    return format == kA1Bgr5Unorm || format == kAbgr4Unorm;
}

// The pack's states that fill a stand-in from such a texture's texels and store it into the texture's words, and the
// format those words are viewed as; none for R8Unorm_sRGB, which the copy engine moves. RGB9E5Float's load reads the words
// as R32Uint and decodes them itself
constexpr const char *formatLoadState(uint32_t format)
{
    switch (format) {
    case kA1Bgr5Unorm:
    case kAbgr4Unorm:
        return "raw.fmt.fmt_load_frag";
    case kRgb9e5Float:
        return "raw.fmt.fmt_load_rgb9e5_frag";
    default:
        return nullptr;
    }
}

constexpr const char *formatStoreState(uint32_t format)
{
    switch (format) {
    case kA1Bgr5Unorm:
        return "raw.fmt.fmt_store_a1bgr5_frag";
    case kAbgr4Unorm:
        return "raw.fmt.fmt_store_abgr4_frag";
    case kRgb9e5Float:
        return "raw.fmt.fmt_store_rgb9e5_frag";
    default:
        return nullptr;
    }
}

constexpr uint32_t formatStoreView(uint32_t format)
{
    return format == kRgb9e5Float ? 53 /* R32Uint */ : 23 /* R16Uint */;
}

// A drawn channel's code (pack/fmt.metal drawnCode): its nearest half, a tie to even, clamped, a NaN 0, then the
// nearest code
inline uint32_t formatDrawnCode(float v, uint32_t max)
{
    const float h = float(_Float16(v));
    if (!(h > 0.0f))
        return 0;
    return uint32_t(std::nearbyint(std::fmin(h, 1.0f) * float(max)));
}

// A cleared channel's code: the nearest code of the float, a tie up
inline uint32_t formatClearCode(float v, uint32_t max)
{
    if (!(v > 0.0f))
        return 0;
    return uint32_t(std::floor(std::fmin(v, 1.0f) * float(max) + 0.5f));
}

// RGB9E5Float of three channels, each clamped to [0, 65408], a NaN 0, by the shared exponent of the largest: mantissas
// truncated (a draw's, pack/fmt.metal rgb9e5Word) or rounded to nearest (a clear's)
inline uint32_t rgb9e5Word(const float in[3], bool rounded)
{
    float c[3];
    for (int k = 0; k < 3; k++)
        c[k] = in[k] > 0.0f ? std::fmin(in[k], 65408.0f) : 0.0f;
    const float m = std::fmax(c[0], std::fmax(c[1], c[2]));
    uint32_t bits;
    memcpy(&bits, &m, 4);
    int e = m > 0.0f ? std::max(0, int(bits >> 23 & 255) - 127 + 16) : 0;
    float scale = std::ldexp(1.0f, e - 24);
    if (rounded && std::floor(m / scale + 0.5f) >= 512.0f) {
        e++;
        scale *= 2;
    }
    uint32_t w = uint32_t(e) << 27;
    for (int k = 0; k < 3; k++)
        w |= uint32_t(rounded ? std::floor(c[k] / scale + 0.5f) : std::floor(c[k] / scale)) << (9 * k);
    return w;
}

inline void rgb9e5Floats(uint32_t w, float out[3])
{
    const float scale = std::ldexp(1.0f, int(w >> 27) - 24);
    for (int k = 0; k < 3; k++)
        out[k] = float(w >> (9 * k) & 511) * scale;
}

// The word the store state writes for a stand-in texel `v`
inline uint32_t formatStoredWord(uint32_t format, const float v[4])
{
    switch (format) {
    case kAbgr4Unorm:
        return formatDrawnCode(v[0], 15) << 12 | formatDrawnCode(v[1], 15) << 8 | formatDrawnCode(v[2], 15) << 4 |
               formatDrawnCode(v[3], 15);
    case kA1Bgr5Unorm:
        return formatDrawnCode(v[0], 31) << 11 | formatDrawnCode(v[1], 31) << 6 | formatDrawnCode(v[2], 31) << 1 |
               formatDrawnCode(v[3], 1);
    case kRgb9e5Float:
        return rgb9e5Word(v, false);
    default:
        return 0;
    }
}

// The colour a stand-in is cleared to for Metal's clear of `in`, which the store state stores as Metal stores the clear. A
// texture Metal keeps losslessly compressed (raw_fresh.h metalCompresses) clears to the clear's own codes: each code the
// nearest, a tie up, RGB9E5's mantissas rounded; the stand-in holds their floats. Any other clears as a draw of the clear's
// colour (probe/fmt/clears.mm: every one of 512 clears by one rule or the other, by usage). R8Unorm_sRGB's clear is the
// RGBA8Unorm_sRGB stand-in's own.
inline void formatStandInClear(uint32_t format, const float in[4], float out[4], bool compressed)
{
    if (compressed && (format == kAbgr4Unorm || format == kA1Bgr5Unorm)) {
        for (int k = 0; k < 4; k++) {
            const uint32_t max = format == kAbgr4Unorm ? 15 : k == 3 ? 1 : 31;
            out[k] = float(formatClearCode(in[k], max)) / float(max);
        }
        return;
    }
    if (compressed && format == kRgb9e5Float) {
        rgb9e5Floats(rgb9e5Word(in, true), out);
        out[3] = 1.0f;
        return;
    }
    for (int k = 0; k < 4; k++)
        out[k] = in[k];
}

// generateMipmapsForTexture: of such a chain: a pass a level and layer of the pack's mip state (pack/fmt.metal
// fmt_mip_*), each texel Metal's step (pack/mip.metal's taps: one bilinear tap at its centre on the level above, the weight
// in 1/256) over the level above's words read as integers, into the level's words viewed as the integers of its texel size
constexpr const char *formatMipState(uint32_t format)
{
    switch (format) {
    case kR8UnormSrgb:
        return "raw.fmt.fmt_mip_srgb8_frag";
    case kA1Bgr5Unorm:
        return "raw.fmt.fmt_mip_a1bgr5_frag";
    case kAbgr4Unorm:
        return "raw.fmt.fmt_mip_abgr4_frag";
    case kRgb9e5Float:
        return "raw.fmt.fmt_mip_rgb9e5_frag";
    default:
        return nullptr;
    }
}

constexpr uint32_t formatMipView(uint32_t format)
{
    return format == kR8UnormSrgb ? 13 /* R8Uint */ : formatStoreView(format);
}

// The mip state's buffer 0: the level above's size and the level's
struct FormatMipStep {
    uint32_t above[2], size[2];
};

// The codes of `max` whose mean at a tie, k + 1/2 over max, is held as a float above it, or exactly at 1/2 with k odd:
// those ties round up (bit k)
constexpr uint32_t formatMipTiesUp(uint32_t max)
{
    return max == 15 ? 0xffu : max == 31 ? 0x5555ff00u : 0u;
}

// An A1BGR5Unorm or ABGR4Unorm channel's step (pack/fmt.metal mipCode): its four codes (the taps' first-first,
// second-first, first-second, second-second) weighted exactly by the 1/256 weights, the mean held as the nearest float, then
// its nearest code, ties even, as an M4 Max stores them (probe/fmt/mipq.mm: 0 of 1365 texels off on 2:1 steps)
inline uint32_t formatMipCode(const uint32_t codes[4], uint32_t wx, uint32_t wy, uint32_t max)
{
    const uint32_t n =
        (codes[0] * (256 - wx) + codes[1] * wx) * (256 - wy) + (codes[2] * (256 - wx) + codes[3] * wx) * wy;
    const uint32_t base = n >> 16, rest = n & 0xffff;
    return base + (rest > 0x8000 || (rest == 0x8000 && (formatMipTiesUp(max) >> base & 1)));
}

struct Texture;
class CommandBuffer;
class Device;

// The stand-in colour `slot` of a pass on `rc` renders `t` into, filled from `t` where `fill`; null where it cannot be made
// (*why)
Texture *formatStandInFor(Device &d, CommandBuffer &rc, Texture &t, uint32_t slot, bool fill, const char **why);
// `t`'s texels from its stand-in after its pass; false: *why
bool storeFormatStandIn(Device &d, CommandBuffer &rc, Texture &t, Texture &standIn, const char **why);
// Levels first + 1 .. last of the selected layers of chain `t` from the level above by the format's mip state; false: *why
bool formatMipPasses(Device &d, CommandBuffer &rc, Texture &t, uint32_t first, uint32_t last, uint32_t firstLayer,
                     uint32_t layers, uint32_t format, const char **why);

}  // namespace raw
