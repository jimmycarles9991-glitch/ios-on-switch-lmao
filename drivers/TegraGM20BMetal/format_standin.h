// SPDX-License-Identifier: MIT
// Colour targets of the formats the GM20B renders none of (FormatInfo rt 0) that Metal renders: R8Unorm_sRGB (11),
// A1BGR5Unorm (41), ABGR4Unorm (42), RGB9E5Float (93) and Apple's extended-range formats (550 to 555, xr.h: the GM20B
// renders their codes as integers, which it cannot blend, and holds no tile of their halves). A pass renders such an
// attachment into a block-linear stand-in of the command buffer's (backing tag kFormatStandInTag + slot), its states made
// for the stand-in's format (TegraGM20BMetalYcbcrTargets.mm standInTargetState; an extended-range one's the descriptor's own,
// which the translator lowers onto the stand-in), filled from the texture before a pass that does not
// clear it and put back after one that stores it. As an M4 Max renders them (probe/fmt: fmtq, edges):
//  - R8Unorm_sRGB holds 8-bit sRGB codes after every draw: an RGBA8Unorm_sRGB stand-in, whose first byte the copy engine
//    moves in and out (BlitEncoder::copyFirstByte); RG8Unorm_sRGB (31) its first two (copyFirstTwoBytes), the GM20B having
//    no two-component sRGB target;
//  - A1BGR5Unorm and ABGR4Unorm hold a pass in halves and store once: a drawn channel's code is rint(clamp(half(v)) * max), a
//    NaN 0, a clear's floor(clamp(v) * max + 0.5). An RGBA32Float stand-in, which the pack's load state fills with the
//    texels' floats and whose floats its store state turns into the codes of a drawn channel, into the texture's words viewed
//    as R16Uint; a clear goes into it as its codes' floats. Blends land within one code: Metal blends halves of its own
//    rounding, the stand-in floats;
//  - RGB9E5Float holds its own words after every draw, each mantissa truncated, a clear's rounded: the same stand-in, the store
//    state truncating, into the words viewed as R32Uint. A pass whose draws do not blend over one another stores Metal's words;
//    blends over earlier draws lie within the truncations Metal makes between them.
//  - the extended-range formats: an RGBA16Float stand-in, Metal's tile (xr.h xrTargetState), which the ROP blends,
//    filled with Metal's blend read of the codes and stored through Metal's write encode into the words viewed as R32Uint
//    (BGR10_XR and its twin), RGBA16Uint (BGRA10_XR's fields) or RG32Uint (a descriptor's RGB10A8_2P_XR10, a word and a
//    byte; one over a surface's two planes from both and into both, the word plane and the alpha plane two targets of the
//    store's pass); a loaded pass's store keeps the texels no draw changed. The sRGB twins' colour codes go through
//    Metal's sRGB tables (xr_srgb.h), which GM20B applies to no 10-bit offset code (nil_formats.csv: 8-bit UNORM channels
//    and sRGB block formats alone).
//  - BGRA4Unorm (44): ABGR4Unorm's rules over NIL's B4G4R4A4_UNORM (blue in the low bits), its header NIL's.
//  - ARGB8Unorm (84) and its sRGB form (85): sampled through RGBA8's header with the channels rotated (its alpha byte first:
//    the header's red source reads alpha); no colour target of that byte order, so an RGBA8 (an sRGB one's RGBA8_sRGB)
//    stand-in the copy engine fills and stores with its component remap, each texel's bytes rotated by one
//    (BlitEncoder::copyArgb8), which keeps every code.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "xr.h"

namespace gm20b {

constexpr uint32_t kFormatStandInTag = 112;
// A resolve target's stand-in's (slot 8 + colour): a multisampled stand-in and its resolve's share no memory
constexpr uint32_t kFormatResolveSlot = 8;

constexpr uint32_t kR8UnormSrgb = 11, kRg8UnormSrgb = 31, kA1Bgr5Unorm = 41, kAbgr4Unorm = 42, kBgra4Unorm = 44,
                   kArgb8Unorm = 84, kArgb8UnormSrgb = 85, kRgb9e5Float = 93, kBgra10XrSrgb = 553, kBgr10XrSrgb = 555;

// The format a pass renders such an attachment into, 0 for any other
constexpr uint32_t formatStandIn(uint32_t format)
{
    if (xrTarget(format))
        return kXrStandInFormat;
    switch (format) {
    case kR8UnormSrgb:
    case kRg8UnormSrgb:
    case kArgb8UnormSrgb:
        return 71;  // RGBA8Unorm_sRGB
    case kArgb8Unorm:
        return 70;  // RGBA8Unorm
    case kA1Bgr5Unorm:
    case kAbgr4Unorm:
    case kBgra4Unorm:
    case kRgb9e5Float:
        return 125;  // RGBA32Float
    default:
        return 0;
    }
}

// Whether the load state reads the texture's words through an integer view of them (formatStoreView), not through its
// own sampled header
constexpr bool formatLoadsWords(uint32_t format)
{
    return format == kRgb9e5Float || xrTarget(format);
}

// Whether the copy engine fills and stores the stand-in (no pack state)
constexpr bool formatStandInCopied(uint32_t format)
{
    return format == kR8UnormSrgb || format == kRg8UnormSrgb || format == kArgb8Unorm || format == kArgb8UnormSrgb;
}

// Whether the format holds alpha: one that does not reads a destination alpha of 1 (standin_blend.h)
constexpr bool formatHoldsAlpha(uint32_t format)
{
    return format == kA1Bgr5Unorm || format == kAbgr4Unorm || format == kBgra4Unorm || format == kArgb8Unorm ||
           format == kArgb8UnormSrgb || (xrTarget(format) && format <= kBgra10XrSrgb);
}

// Whether the store state of such an attachment reads its codes (texture 1, a surface's alpha plane texture 2) and keeps
// those of a loaded pass that no draw changed (buffer 0: a uint, non-zero to keep): an extended-range one's (xr.h)
constexpr bool formatStoreKeeps(uint32_t format)
{
    return formatStandIn(format) && xrTarget(format);
}

// A state by the sample count of the pass it draws in: one sample's, else the per-sample state of a 2- or 4-sample pass
// (texture2d_ms reads at sample rate, a state keyed samples=2 or 4: m2v-passes' pack states, these names their contract);
// null for any other count
constexpr const char *bySamples(uint32_t samples, const char *one, const char *two, const char *four)
{
    return samples <= 1 ? one : samples == 2 ? two : samples == 4 ? four : nullptr;
}

// The pack's states that fill a stand-in from such a texture's texels and store it into the texture's words, and the
// format those words are viewed as; none for the formats the copy engine moves (formatStandInCopied). RGB9E5Float's load
// reads the words as R32Uint and decodes them itself
constexpr const char *formatLoadState(uint32_t format, uint32_t samples = 1)
{
    if (formatStoreKeeps(format))
        return xrTargetState(format, XrStep::Load, samples);
    switch (format) {
    case kA1Bgr5Unorm:
    case kAbgr4Unorm:
    case kBgra4Unorm:
        return bySamples(samples, "raw.fmt.fmt_load_frag", "raw.fmt.fmt_load_ms2_frag", "raw.fmt.fmt_load_ms4_frag");
    case kRgb9e5Float:
        return bySamples(samples, "raw.fmt.fmt_load_rgb9e5_frag", "raw.fmt.fmt_load_rgb9e5_ms2_frag",
                         "raw.fmt.fmt_load_rgb9e5_ms4_frag");
    default:
        return nullptr;
    }
}

constexpr const char *formatStoreState(uint32_t format, uint32_t samples = 1)
{
    if (formatStoreKeeps(format))
        return xrTargetState(format, XrStep::Store, samples);
    switch (format) {
    case kA1Bgr5Unorm:
        return bySamples(samples, "raw.fmt.fmt_store_a1bgr5_frag", "raw.fmt.fmt_store_a1bgr5_ms2_frag",
                         "raw.fmt.fmt_store_a1bgr5_ms4_frag");
    case kAbgr4Unorm:
        return bySamples(samples, "raw.fmt.fmt_store_abgr4_frag", "raw.fmt.fmt_store_abgr4_ms2_frag",
                         "raw.fmt.fmt_store_abgr4_ms4_frag");
    case kBgra4Unorm:
        return bySamples(samples, "raw.fmt.fmt_store_bgra4_frag", "raw.fmt.fmt_store_bgra4_ms2_frag",
                         "raw.fmt.fmt_store_bgra4_ms4_frag");
    case kRgb9e5Float:
        return bySamples(samples, "raw.fmt.fmt_store_rgb9e5_frag", "raw.fmt.fmt_store_rgb9e5_ms2_frag",
                         "raw.fmt.fmt_store_rgb9e5_ms4_frag");
    default:
        return nullptr;
    }
}

constexpr uint32_t formatStoreView(uint32_t format)
{
    return format == 552 || format == kBgra10XrSrgb    ? 113 /* RGBA16Uint */
           : format == 550 || format == kRgb10A8XrSrgb ? 103 /* RG32Uint */
           : formatLoadsWords(format)                  ? 53  /* R32Uint */
                                                       : 23 /* R16Uint */;
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
    case kBgra4Unorm:
        return formatDrawnCode(v[3], 15) << 12 | formatDrawnCode(v[0], 15) << 8 | formatDrawnCode(v[1], 15) << 4 |
               formatDrawnCode(v[2], 15);
    case kRgb9e5Float:
        return rgb9e5Word(v, false);
    default:
        return 0;
    }
}

// The colour a stand-in is cleared to for Metal's clear of `in`, which the store state stores as Metal stores the clear. A
// texture Metal keeps losslessly compressed (fresh.h metalCompresses) clears to the clear's own codes: each code the
// nearest, a tie up, RGB9E5's mantissas rounded; the stand-in holds their floats. Any other clears as a draw of the clear's
// colour (probe/fmt/clears.mm: every one of 512 clears by one rule or the other, by usage). R8Unorm_sRGB's clear is the
// RGBA8Unorm_sRGB stand-in's own. An extended-range format's tile holds a clear as each channel's nearest half, a format
// of no alpha's alpha 1: a draw's route. Metal's straight route (a compressed
// texture's pass that records no draw) clears BGR10_XR's and BGRA10_XR's codes through the clear registers
// (TegraGM20BMetalRangeForms.mm formatStandIns); the sRGB twins' is not told apart, and stores within one code of it.
inline void formatStandInClear(uint32_t format, const float in[4], float out[4], bool compressed)
{
    if (xrTarget(format)) {
        for (int k = 0; k < 4; k++)
            out[k] = float(_Float16(in[k]));
        if (!formatHoldsAlpha(format))
            out[3] = 1.0f;
        return;
    }
    if (compressed && (format == kAbgr4Unorm || format == kBgra4Unorm || format == kA1Bgr5Unorm)) {
        for (int k = 0; k < 4; k++) {
            const uint32_t max = format != kA1Bgr5Unorm ? 15 : k == 3 ? 1 : 31;
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

// generateMipmapsForTexture: of such a chain: a pass a level and layer of the pack's mip state (pack/fmt.metal WORD_MIP),
// each texel Metal's step over the level above's words read as integers, into the level's words viewed as the integers of
// its texel size
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

struct Texture;
struct ColorAttachment;
class CommandBuffer;
class Device;

// The stand-in `slot` (the colour, or kFormatResolveSlot + it for its resolve target) of a pass on `rc` renders attachment
// `a` into (as the pass bound it: its texture, and the image, level and slice it is a part of), `layers` layers (a 3D
// level's planes) of `a`'s sample count from there; filled from them where `fill`; null where it cannot be made (*why).
// Block-linear scratch of the command buffer's, one layer after another layerBytes apart, filled a layer at a time: a
// multisampled one by the per-sample load state, the copy engine's formats over the sample grid. A resolve into a texture
// of such a format: the 3D engine's resolve of this stand-in into the resolve target's own (`fill` false), then
// storeFormatStandIn of that
Texture *formatStandInFor(Device &d, CommandBuffer &rc, const ColorAttachment &a, uint32_t layers, uint32_t slot,
                          bool fill, const char **why);
// `a`'s texels from its stand-in after its pass, a layer at a time (a multisampled one by the per-sample store state);
// `cleared`: the pass drew nothing (xr_pools.h storeXrPlaneRows); `keep`: the stand-in was filled from the texels, whose
// store keeps those no draw changed (formatStoreKeeps); false: *why
bool storeFormatStandIn(Device &d, CommandBuffer &rc, const ColorAttachment &a, uint32_t layers, Texture &standIn,
                        bool cleared, const char **why, bool keep = false);
// Levels first + 1 .. last of the selected layers of chain `t` from the level above by the format's mip state; false: *why
bool formatMipPasses(Device &d, CommandBuffer &rc, Texture &t, uint32_t first, uint32_t last, uint32_t firstLayer,
                     uint32_t layers, uint32_t format, const char **why);

}  // namespace gm20b
