// SPDX-License-Identifier: MIT
// A pass that loads a two-plane YCbCr target (formats 500 to 548), as an M4 Max runs one (raw/test/ycbcrloadoracle.mm):
//  - the Load holds each channel the texture reads (pack/ycbcr.metal's conversion) as the nearest half, a tie rounded up (a
//    code format's quotient is never a tie, an sRGB step can be, a converted channel's v / 2048 is a half already);
//  - the store writes only the 32x32 tiles of the pass's frame a draw reaches, by raw_reach.h's rule (nothing in a pass
//    without a draw, every tile beside another attachment cleared and stored), each from its texels by the store rule
//    (raw_ycbcr_store.h); a chroma block lies in its first luma texel's tile, and every other byte keeps its value;
//  - its passes into a packed target fault, so a packed target's words are its unpacked twin's codes, three to a word, each
//    code of a tile no draw reaches the word's own.
// The raw path draws the load state's values into a block-linear stand-in of the command buffer's (backing tag kYcbcrLoadTag +
// colour), which the pass renders into, and its tile-masked store states draw the planes through the pass's reach mask.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "raw_reach.h"
#include "raw_ycbcr.h"
#include "raw_ycbcr_store.h"

namespace raw {

constexpr uint32_t kYcbcrLoadTag = 96;

// pack/ycbcr_store.metal Tiles: the reach mask's tiles across and down, bound at the masked states' buffer(2)
struct YcbcrStoreTiles {
    uint32_t x = 0, y = 0;
};

constexpr uint32_t kYcbcrTilesSlot = 2;

// The pack's load state of a texture read as `output` (YcbcrOutput); null for none (a packed 10-bit plane's is
// ycbcrLoadPipeline's)
inline const char *ycbcrLoadState(uint8_t output)
{
    switch (output) {
    case YcbcrConverted:
        return "raw.ycbcr.ycbcr_load_converted_frag";
    case YcbcrSrgb:
        return "raw.ycbcr.ycbcr_load_srgb_frag";
    case YcbcrCodes8:
    case YcbcrCodes10:
        return "raw.ycbcr.ycbcr_load_codes_frag";
    case YcbcrConvertedPacked:
        return "raw.ycbcr.ycbcr_load_packed_converted_frag";
    case YcbcrSrgbPacked:
        return "raw.ycbcr.ycbcr_load_packed_srgb_frag";
    case YcbcrCodes10Packed:
        return "raw.ycbcr.ycbcr_load_packed_codes10_frag";
    default:
        return nullptr;
    }
}

// The pack's tile-masked store state of a target's luma or chroma plane (the plane-packed formats have none: their load and
// store give each code back)
inline const char *ycbcrMaskedStoreState(bool chroma, const YcbcrStoreShape &s)
{
    static const char *const names[2][2][2] = {
        {{"raw.ycbcr_store.ycbcr_store_luma8_masked_frag", "raw.ycbcr_store.ycbcr_store_luma16_masked_frag"},
         {"raw.ycbcr_store.ycbcr_store_chroma8_masked_frag", "raw.ycbcr_store.ycbcr_store_chroma16_masked_frag"}},
        {{"raw.ycbcr_store.ycbcr_store_luma8_srgb_masked_frag", "raw.ycbcr_store.ycbcr_store_luma16_srgb_masked_frag"},
         {"raw.ycbcr_store.ycbcr_store_chroma8_srgb_masked_frag",
          "raw.ycbcr_store.ycbcr_store_chroma16_srgb_masked_frag"}}};
    static const char *const packed[2][2] = {{"raw.ycbcr_store.ycbcr_store_luma_packed_masked_frag",
                                              "raw.ycbcr_store.ycbcr_store_chroma_packed_masked_frag"},
                                             {"raw.ycbcr_store.ycbcr_store_luma_packed_srgb_masked_frag",
                                              "raw.ycbcr_store.ycbcr_store_chroma_packed_srgb_masked_frag"}};
    if (s.plane)
        return nullptr;
    return s.packed ? packed[s.srgb != 0][chroma] : names[s.srgb != 0][chroma][s.max > 255];
}

// The half nearest num / den (num <= den), a tie rounded up (pack/ycbcr.metal loadHalf)
inline float ycbcrLoadHalf(uint32_t num, uint32_t den)
{
    if (!num)
        return 0.0f;
    uint32_t p = 10 + uint32_t(__builtin_clz(num) - __builtin_clz(den));
    if ((num << p) < (den << 10))
        p++;
    return std::ldexp(float(((num << p) * 2 + den) / (2 * den)), -int(p));
}

// A texture's plane codes as the conversion reads them: luma(x, y) the code of luma texel (x, y), chroma(x, y, c) code c (0 Cb,
// 1 Cr) of chroma pair (x, y), each already the code (a byte, a 16-bit word's high bits, a packed word's slot)
struct YcbcrPlaneCodes {
    const uint16_t *luma = nullptr;    // width x height
    const uint16_t *chroma = nullptr;  // (pairs across) x (rows) pairs, Cb then Cr
    uint32_t pairsAcross = 0;
};

// The texel's luma code and its filtered Cb and Cr codes (pack/ycbcr.metal codes over a shape of kYcbcrShapeWords words)
inline void ycbcrReadCodes(const uint32_t *shape, const YcbcrPlaneCodes &p, uint32_t x, uint32_t y, uint32_t out[3])
{
    const uint32_t width = shape[0], height = shape[1], halved = shape[13];
    out[0] = p.luma[y * width + x];
    out[1] = out[2] = 0;
    if (!shape[14])
        return;
    const auto taps = [](uint32_t at, uint32_t n, bool half, uint32_t t[3]) {
        if (!half) {
            t[0] = t[1] = at;
            t[2] = 4;
            return;
        }
        const uint32_t c = at >> 1, odd = at & 1, a = c + odd, last = n - 1;
        t[0] = odd ? c : (a == 0 ? 0 : a - 1);
        t[1] = std::min(a, last);
        t[2] = odd ? 3 : 1;
    };
    uint32_t tx[3], ty[3];
    taps(x, (halved & 1) ? (width + 1) >> 1 : width, halved & 1, tx);
    taps(y, (halved & 2) ? (height + 1) >> 1 : height, halved & 2, ty);
    uint32_t cb = 0, cr = 0;
    for (uint32_t j = 0; j < 2; j++)
        for (uint32_t i = 0; i < 2; i++) {
            const uint32_t w = (i ? 4 - tx[2] : tx[2]) * (j ? 4 - ty[2] : ty[2]);
            const uint16_t *pair = &p.chroma[2 * ((j ? ty[1] : ty[0]) * p.pairsAcross + (i ? tx[1] : tx[0]))];
            cb += w * pair[0];
            cr += w * pair[1];
        }
    out[1] = (cb + 8) >> 4;
    out[2] = (cr + 8) >> 4;
}

// A loaded pass's stand-in texel (red, green, blue, alpha) of a texture read as `output` (pack/ycbcr.metal's load states);
// `knots` pack/srgb_knots.h for an sRGB one
inline void ycbcrLoadTexel(uint8_t output, const uint32_t *shape, const YcbcrPlaneCodes &p, const uint16_t *knots,
                           uint32_t x, uint32_t y, float out[4])
{
    uint32_t c[3];
    ycbcrReadCodes(shape, p, x, y, c);
    out[3] = 1.0f;
    if (output == YcbcrCodes8 || output == YcbcrCodes10 || output == YcbcrCodes10Packed) {
        out[0] = ycbcrLoadHalf(c[2], shape[12]);
        out[1] = ycbcrLoadHalf(c[0], shape[12]);
        out[2] = ycbcrLoadHalf(c[1], shape[12]);
        return;
    }
    int32_t k[6];
    memcpy(k, shape + 3, sizeof k);
    const uint32_t up = shape[11], max = shape[12];
    const int32_t luma = shape[2] ? 256 * int32_t((c[0] * 32768 + max / 2) / max) : 9576 * (int32_t(c[0] << up) - 64);
    const int32_t cb = int32_t(c[1] << up) - 512, cr = int32_t(c[2] << up) - 512;
    for (uint32_t ch = 0; ch < 3; ch++) {
        const int32_t t = luma + k[2 * ch] * cb + k[2 * ch + 1] * cr;
        if (output == YcbcrSrgb || output == YcbcrSrgbPacked) {
            const uint32_t q = uint32_t(std::clamp((t + 512) >> 10, 0, int32_t(kYcbcrSrgbSteps)));
            out[ch] = ycbcrLoadHalf(ycbcrSrgbDecodeStep(knots, q), kYcbcrSrgbSteps);
        } else {
            out[ch] = ycbcrLoadHalf(uint32_t(std::clamp((t + 2048) >> 12, 0, 2048)), 2048);
        }
    }
}

// Whether a draw reaches frame pixel (x, y): `mask` a byte a 32x32 tile, `tilesX` a row (raw_reach.h)
inline bool ycbcrTileReached(const uint8_t *mask, uint32_t tilesX, uint32_t tilesY, uint32_t x, uint32_t y)
{
    const uint32_t tx = x / kReachTile, ty = y / kReachTile;
    return tx < tilesX && ty < tilesY && mask[ty * tilesX + tx];
}

// What Metal's tile holds of a code target's planes after a Load (ycbcrLoadTexel's rule): `rgba` (width x height texels,
// four halves each as floats) of `luma` (a code a texel) and `chroma` ((width + 1) / 2 x (height / 2 or height) pairs, Cb
// then Cr)
inline void ycbcrLoaded(const YcbcrCodeTarget &t, const uint16_t *luma, const uint16_t *chroma, uint32_t width,
                        uint32_t height, float *rgba)
{
    uint32_t shape[kYcbcrShapeWords] = {width, height};
    shape[12] = t.max;
    shape[13] = t.halvedDown ? 3 : 1;
    shape[14] = 1;
    const YcbcrPlaneCodes codes = {luma, chroma, (width + 1) / 2};
    const uint8_t output = t.max > 255 ? YcbcrCodes10 : YcbcrCodes8;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            ycbcrLoadTexel(output, shape, codes, nullptr, x, y, rgba + 4 * (size_t(y) * width + x));
}

// Metal's codes after a pass that loads a code target's planes and draws `green` (a float a texel) through write mask green:
// the store of the loaded halves with green drawn over them. False for a size the chroma blocks do not tile.
inline bool ycbcrStoreOverLoad(const YcbcrCodeTarget &t, const uint16_t *luma, const uint16_t *chroma,
                               const float *green, uint32_t width, uint32_t height, uint16_t *lumaOut,
                               uint16_t *chromaOut)
{
    std::vector<float> rgba(size_t(width) * height * 4);
    ycbcrLoaded(t, luma, chroma, width, height, rgba.data());
    for (size_t i = 0; i < size_t(width) * height; i++)
        rgba[4 * i + 1] = green[i];
    return ycbcrStore(t, rgba.data(), width, height, YcbcrTexelSource::DrawnFloat, lumaOut, chromaOut);
}

}  // namespace raw
