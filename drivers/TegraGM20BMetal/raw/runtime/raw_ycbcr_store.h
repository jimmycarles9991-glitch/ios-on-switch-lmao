// SPDX-License-Identifier: MIT
// The raw path's route of a render pass into a two-plane YCbCr attachment, to Metal's store (raw_ycbcr_store_rule.h): the pass
// renders into a stand-in, then the pack's store states (pack/ycbcr_store.metal) draw the planes from it, every code worked out
// in the program as ycbcrStoreDrawn works it out here, step for step.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "raw_ycbcr_store_rule.h"

namespace raw {

// The format a pass of the raw path renders such an attachment's texels into, its pipelines made for it in the attachment's
// place: RGBA32Float, the fragment's floats as they are, which the store states read as halves rounded as Metal holds them. An
// RGBA16Float target would hold them rounded toward zero on the GM20B (HW Test metal.rgba16f_rounding: 0.5 - 2^-13 stored as
// 0x37ff). A pass's stand-in is a memoryless attachment: its backing is the pass's, read by the store passes after it.
constexpr uint32_t kYcbcrTargetStandIn = 125;

// The fragment buffer slot the _srgb store states read pack/srgb_encode.h from (Device::srgbEncodeTable)
constexpr uint32_t kSrgbEncodeSlot = 1;
constexpr uint32_t kSrgbEncodeEntries = 8193;

// R10Unorm_PACKED's and RG10Unorm_PACKED's codes a texel (raw_packed_plane.h), 0 for any other format
inline uint32_t packedPlaneChannels(uint32_t pixelFormat)
{
    return pixelFormat == 588 ? 1 : pixelFormat == 589 ? 2 : 0;
}

// The format a pass renders an attachment of `pixelFormat` into, its pipelines made for it: the YCbCr formats' stand-in, or
// a packed plane's R32Float or RG32Float. A fragment function returns no fewer components than its attachment's format has
// (Metal's pipeline error, which m2v refuses), and R10Unorm_PACKED's and RG10Unorm_PACKED's return one or two.
inline uint32_t ycbcrStandInFormat(uint32_t pixelFormat)
{
    switch (packedPlaneChannels(pixelFormat)) {
    case 1:
        return 55;  // R32Float
    case 2:
        return 105;  // RG32Float
    default:
        return kYcbcrTargetStandIn;
    }
}

// A format whose passes the raw path renders through its stand-in, or refuses (ycbcrTargetRefusal): the two-plane YCbCr
// formats but RGB8_444_2P, which the frontend renders through its own stand-in, and the packed 10-bit planes
inline bool ycbcrRenderedFormat(uint32_t pixelFormat)
{
    return (pixelFormat >= 500 && pixelFormat <= 548 && pixelFormat != 542) || packedPlaneChannels(pixelFormat);
}

// The MTLColorWriteMask bits a state's attachment of such a format writes for its stand-in to hold every channel the
// store reads: red, green and blue, or the packed plane's red (and green)
inline uint32_t ycbcrTargetWrites(uint32_t pixelFormat)
{
    constexpr uint32_t kRed = 8, kGreen = 4, kBlue = 2;
    switch (packedPlaneChannels(pixelFormat)) {
    case 1:
        return kRed;
    case 2:
        return kRed | kGreen;
    default:
        return kRed | kGreen | kBlue;
    }
}

// Null where the raw path renders passes into a two-plane YCbCr attachment of `pixelFormat` (its stand-in, then the pack's
// store states into the planes: pack/ycbcr_store.metal), else why it cannot
inline const char *ycbcrTargetRefusal(uint32_t pixelFormat)
{
    switch (pixelFormat) {
    case 500:
    case 502:
    case 503:
    case 505:
    case 506:
    case 507:
    case 508:
    case 509:
    case 510:
    case 520:
    case 522:
    case 523:
    case 525:
    case 526:
    case 527:
    case 528:
    case 529:
    case 530:
    case 540:
    case 541:
    case 543:
    case 544:
    case 545:
    case 546:
    case 547:
    case 548:
    case 588:
    case 589:
        return nullptr;
    default:
        return "not a two-plane YCbCr format the raw path renders";
    }
}

// The pack's store states' fragment buffer 0 (pack/ycbcr_store.metal StoreShape)
struct YcbcrStoreShape {
    uint32_t max = 0, shift = 0, towardZero = 0, across = 0, down = 0, converted = 0;
    int32_t offset[3] = {};
    int32_t row[3][3] = {};
    uint32_t mul[3] = {};
    uint32_t termBits = 6;
    uint32_t srgb = 0;    // the _srgb states': steps through pack/srgb_encode.h, bound at kSrgbEncodeSlot
    uint32_t packed = 0;  // the _packed states: R32Uint planes of three codes a word
    uint32_t plane = 0;   // the _plane_packed state: a packed 10-bit plane's codes a texel (raw_packed_plane.h)
};

static_assert(sizeof(YcbcrStoreShape) == 100, "pack/ycbcr_store.metal StoreShape");

// A code target of `bits`-bit codes: 10-bit codes in a 16-bit word's high bits, a drawn float held toward zero
inline YcbcrStoreShape ycbcrCodesShape(uint32_t bits, bool across, bool down)
{
    YcbcrStoreShape s;
    s.max = (1u << bits) - 1;
    s.shift = bits == 10 ? 6 : 0;
    s.towardZero = bits == 10;
    s.across = across;
    s.down = down;
    return s;
}

// A converted or sRGB target: ycbcrConvertedRows' rows as a 15-bit coefficient and its scale over 2^20
inline YcbcrStoreShape ycbcrConvertedShape(const YcbcrConvertedTarget &t)
{
    YcbcrStoreShape s = ycbcrCodesShape(t.bits, t.across, t.down);
    s.converted = 1;
    s.termBits = t.bits == 10 ? 4 : 6;
    s.srgb = t.srgb;
    const int i = int(t.matrix), ten = t.bits == 10 ? 2 : 0;
    for (int code = 0; code < 3; code++) {
        s.offset[code] = code ? 1 << (t.bits - 1) : t.full ? 0 : 16 << (t.bits - 8);
        for (int c = 0; c < 3; c++)
            s.row[code][c] = t.full ? kYcbcrRowsQ15[i][code][c] : kYcbcrRowsVideo[i][code][c];
        s.mul[code] =
            t.full ? (code ? 1u << t.bits : (1u << t.bits) - 1) : 1u << (26 - kYcbcrRowsVideo[i][code][3] + ten);
    }
    return s;
}

// A packed target (YCBCR10_*_PACKED, their sRGB twins, RGB10_*_PACKED): its unpacked twin's codes (`twin`), three to a word
inline YcbcrStoreShape ycbcrPackedShape(YcbcrStoreShape twin)
{
    twin.shift = 0;
    twin.packed = 1;
    return twin;
}

// `n` codes of a packed row in its words, as the _packed states write them: code x at bit 10 (x % 3) of word x / 3, the slots
// past the last code and each word's top two bits 0 (the reads' layout, m2v bind/ycbcr_convert.rs; Metal's store of a packed
// target is not measured: an M4 Max's command buffer faults). Nothing reads those spare bits: Metal's packed views,
// VTPixelTransferSession and Core Image read the same with them set (raw/test/packedreaders.mm on macOS 27.2.1, HW Test
// gpu.packed_spare_bits on the device), and CoreVideo's black blocks of 'p420', 'p422' and 'p444' hold them 0
inline void ycbcrPackRow(const uint16_t *codes, uint32_t n, uint32_t *words)
{
    for (uint32_t w = 0; w < (n + 2) / 3; w++) {
        words[w] = 0;
        for (uint32_t i = 0; i < 3 && 3 * w + i < n; i++)
            words[w] |= uint32_t(codes[3 * w + i] & 1023) << (10 * i);
    }
}

// floor(sum * mul / 2^20) as the store state takes it in 32-bit steps: sum = 4096 q + r, q mul = 256 s + u (|sum| <= 2^28,
// mul <= 2048)
inline int32_t ycbcrScaled(int32_t sum, uint32_t mul)
{
    const int32_t q = sum >> 12, r = sum & 4095, qm = q * int32_t(mul);
    return (qm >> 8) + (((qm & 255) * 4096 + r * int32_t(mul)) >> 20);
}

// A stand-in channel's code as the store states take it (pack/ycbcr_store.metal texelCode): held as a half, toward zero
// for a 10-bit code, clamped to [0, 1] (a NaN 0), times max rounded half up
inline uint32_t ycbcrStoreCode(const YcbcrStoreShape &s, float v)
{
    if (std::isnan(v) || v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return s.max;
    return uint32_t(std::floor(ycbcrHeldHalf(v, s.towardZero != 0) * s.max + 0.5));
}

// The store states' codes of a pass's stand-in (`rgba`, width x height texels): the luma plane's and each chroma block's Cb, Cr
// as the pack's ycbcr_store.metal computes them, step for step. A converted channel's step is on the grid of 1/8192: 4 rint(h
// 2048), or an sRGB target's E[rint(h 8192)] (`encode`).
inline bool ycbcrStoreDrawn(const YcbcrStoreShape &s, const uint16_t *encode, const float *rgba, uint32_t width,
                            uint32_t height, uint16_t *luma, uint16_t *chroma)
{
    const uint32_t w = s.across ? 2 : 1, h = s.down ? 2 : 1, n = w * h, k = (s.across ? 1 : 0) + (s.down ? 1 : 0);
    if (!width || !height || width % w || height % h)
        return false;
    const auto held = [&](float v) {
        return ycbcrHeldHalf(v, s.towardZero != 0);
    };
    const auto code = [&](float v) {
        return ycbcrStoreCode(s, v);
    };
    const auto step = [&](float v) {
        if (std::isnan(v) || v <= 0.0f)
            return 0;
        if (v >= 1.0f)
            return int32_t(kYcbcrSrgbSteps);
        return s.srgb ? int32_t(encode[uint32_t(std::nearbyint(held(v) * kYcbcrSrgbSteps))])
                      : 4 * int32_t(std::nearbyint(held(v) * 2048.0));
    };
    const auto term = [&](uint32_t c, uint32_t texel) {
        int32_t sum = 0;
        for (uint32_t ch = 0; ch < 3; ch++)
            sum += s.row[c][ch] * step(rgba[4 * texel + ch]);
        return ycbcrScaled(sum, s.mul[c]) >> (8 - s.termBits);
    };
    const auto encoded = [&](uint32_t c, int32_t sum, uint32_t blockLog2) {
        const uint32_t shift = s.termBits + blockLog2;
        return uint16_t(std::clamp(s.offset[c] + ((sum + (1 << (shift - 1))) >> shift), 0, int32_t(s.max)));
    };
    for (uint32_t i = 0; i < width * height; i++)
        luma[i] = s.converted ? encoded(0, term(0, i), 0) : uint16_t(code(rgba[4 * i + 1]));
    const uint32_t cw = width / w, ch = height / h;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++) {
            int32_t cb = 0, cr = 0;
            for (uint32_t dy = 0; dy < h; dy++)
                for (uint32_t dx = 0; dx < w; dx++) {
                    const uint32_t texel = (y * h + dy) * width + x * w + dx;
                    cb += s.converted ? term(1, texel) : int32_t(code(rgba[4 * texel + 2]));
                    cr += s.converted ? term(2, texel) : int32_t(code(rgba[4 * texel]));
                }
            uint16_t *out = &chroma[2 * (y * cw + x)];
            out[0] = s.converted ? encoded(1, cb, k) : uint16_t((uint32_t(cb) + n / 2) / n);
            out[1] = s.converted ? encoded(2, cr, k) : uint16_t((uint32_t(cr) + n / 2) / n);
        }
    return true;
}

// The _packed states' words of a pass's stand-in, as pack/ycbcr_store.metal packedLuma and packedChroma assemble them over
// ycbcrStoreDrawn's codes: luma word x of a row holds codes 3 x .. 3 x + 2, chroma word x the codes of the pairs of blocks
// 3 x / 2 and the one after it, Cb at an even code and Cr at an odd one, a slot past the row's last code 0
inline bool ycbcrStoreDrawnPacked(const YcbcrStoreShape &s, const uint16_t *encode, const float *rgba, uint32_t width,
                                  uint32_t height, uint32_t *lumaWords, uint32_t *chromaWords)
{
    const uint32_t blocks = s.across ? width / 2 : width, rows = s.down ? height / 2 : height;
    std::vector<uint16_t> luma(size_t(width) * height), chroma(2 * size_t(blocks) * rows);
    if (!ycbcrStoreDrawn(s, encode, rgba, width, height, luma.data(), chroma.data()))
        return false;
    const uint32_t lumaRow = (width + 2) / 3, chromaRow = (2 * blocks + 2) / 3;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < lumaRow; x++) {
            uint32_t word = 0;
            for (uint32_t i = 0; i < 3 && 3 * x + i < width; i++)
                word |= uint32_t(luma[y * width + 3 * x + i]) << (10 * i);
            lumaWords[y * lumaRow + x] = word;
        }
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < chromaRow; x++) {
            const uint32_t first = 3 * x, block = first / 2;
            const uint16_t *pairs[2] = {block < blocks ? &chroma[2 * (y * blocks + block)] : nullptr,
                                        block + 1 < blocks ? &chroma[2 * (y * blocks + block + 1)] : nullptr};
            uint32_t word = 0;
            for (uint32_t i = 0; i < 3; i++) {
                const uint32_t code = first + i;
                if (const uint16_t *pair = pairs[code / 2 - block])
                    word |= uint32_t(pair[code & 1]) << (10 * i);
            }
            chromaWords[y * chromaRow + x] = word;
        }
    return true;
}

}  // namespace raw
