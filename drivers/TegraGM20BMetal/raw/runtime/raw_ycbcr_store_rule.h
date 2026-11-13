// SPDX-License-Identifier: MIT
// Metal's store of a render pass into a two-plane YCbCr attachment over plane 0 of a CoreVideo surface of its own subsampling,
// measured on an M4 Max (raw/test/ycbcrstoreoracle.mm: clears, float and half draws, every byte of both planes; macOS 27.0.1
// for the code formats, 27.2.1 for the converted 4:4:4 and 10-bit ones and the sRGB ones, over no matrix, BT.601, BT.709,
// SMPTE 240M and BT.2020).
//
// Each texel is held as a half: a drawn float rounded to nearest even (8-bit codes) or toward zero (10-bit), a half a fragment
// returns kept, a clear's colour rounded to nearest even. A held channel is clamped to [0, 1], a NaN 0. A 10-bit code is a
// little-endian 16-bit word's high ten bits, its low six 0. Plane 0 holds each texel's luma code, plane 1 each chroma block's
// Cb then Cr code: 2 x 2 texels at 4:2:0, 2 x 1 at 4:2:2, its own at 4:4:4.
//
//  - codes (RGB*_2P: 540, 541, 543, 544): luma the green code, Cb the blue, Cr the red, each round(h * M) (M = 255 or 1023);
//    a block's code its texels' codes' mean rounded half up, (sum + n / 2) / n;
//  - converted (YCBCR*_2P: 500, 502, 503, 505, 506, 507; m2v bind/ycbcr_write.rs): a channel's step x = rint(h * 2048), each
//    texel's term of a code's row c (red, green, blue, counted at 2^26) t = (c . x) >> (20 + b - 8), truncated at 2^-T of a
//    code (T = 6 at 8 bits, 4 at 10), the code o + ((sum t + 2^(T - 1 + k)) >> (T + k)) over its block of 2^k texels, clamped;
//  - sRGB (YCBCR*_2P_sRGB: 520, 522, 523, 525, 526, 527): the same rows and blocks over x = E[rint(h * 8192)], a step of 1/8192,
//    t = (c . x) >> (22 + b - 8). E is the inverse of the read's decode D (pack/ycbcr.metal srgbStep: the knots joined by lines,
//    rounded to nearest even): of the sRGB steps whose D is nearest q, the middle one rounded up, and E[0] = 0
//    (ycbcrSrgbEncodeTable; every one of the 4097 steps a held half reaches pinned by the measured codes).
//
// A Load reloads the chroma between blocks interpolated (a block reloaded and stored again moves), and a DontCare leaves
// undrawn texels undefined (of a partial draw's, the M4 Max kept 77% of the bytes and stored zeros over the rest): neither has a
// rule here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace raw {

struct YcbcrCodeTarget {
    uint32_t max = 0;  // 255 or 1023
    bool halvedDown = false;
};

inline bool ycbcrCodeTarget(uint32_t pixelFormat, YcbcrCodeTarget &t)
{
    switch (pixelFormat) {
    case 540:
        t = {255, true};
        return true;
    case 541:
        t = {255, false};
        return true;
    case 543:
        t = {1023, true};
        return true;
    case 544:
        t = {1023, false};
        return true;
    default:
        return false;
    }
}

enum class YcbcrTexelSource : uint8_t {
    DrawnFloat,
    DrawnHalf,
    Clear
};

// The half Metal holds of `v` in (0, 1), as a double (exact)
inline double ycbcrHeldHalf(float v, bool towardZero)
{
    int e = 0;
    std::frexp(double(v), &e);
    const double step = std::ldexp(1.0, std::max(e - 1, -14) - 10);
    const double q = double(v) / step;
    return (towardZero ? std::floor(q) : std::nearbyint(q)) * step;
}

// A channel as Metal holds it, clamped to [0, 1], a NaN 0
inline double ycbcrHeld(float v, YcbcrTexelSource source, uint32_t bits)
{
    if (std::isnan(v) || v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 1;
    if (source == YcbcrTexelSource::DrawnHalf)
        return v;
    return ycbcrHeldHalf(v, bits == 10 && source == YcbcrTexelSource::DrawnFloat);
}

// A held channel's code
inline uint32_t ycbcrTexelCode(float v, YcbcrTexelSource source, uint32_t max)
{
    return uint32_t(std::floor(ycbcrHeld(v, source, max == 1023 ? 10 : 8) * max + 0.5));
}

// A chroma code of its block's `n` texel codes (2 or 4)
inline uint32_t ycbcrBlockCode(uint32_t sum, uint32_t n)
{
    return (sum + n / 2) / n;
}

// Metal's codes of a pass's store of `rgba` (width x height texels, four floats each, row by row): `luma` width x height,
// `chroma` (width / 2) x (height / 2 or height) pairs, Cb then Cr. False for a size the chroma blocks do not tile.
inline bool ycbcrStore(const YcbcrCodeTarget &t, const float *rgba, uint32_t width, uint32_t height,
                       YcbcrTexelSource source, uint16_t *luma, uint16_t *chroma)
{
    const uint32_t down = t.halvedDown ? 2 : 1;
    if (!width || !height || width % 2 || height % down)
        return false;
    for (uint32_t i = 0; i < width * height; i++)
        luma[i] = uint16_t(ycbcrTexelCode(rgba[4 * i + 1], source, t.max));
    const uint32_t cw = width / 2, ch = height / down;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++)
            for (uint32_t c = 0; c < 2; c++) {
                const uint32_t channel = c ? 0 : 2;
                uint32_t sum = 0;
                for (uint32_t dy = 0; dy < down; dy++)
                    for (uint32_t dx = 0; dx < 2; dx++)
                        sum +=
                            ycbcrTexelCode(rgba[4 * ((y * down + dy) * width + 2 * x + dx) + channel], source, t.max);
                chroma[2 * (y * cw + x) + c] = uint16_t(ycbcrBlockCode(sum, 2 * down));
            }
    return true;
}

enum class YcbcrMatrix : uint8_t {
    Bt601,
    Bt709,
    Bt2020
};

constexpr uint32_t kYcbcrSrgbSteps = 8192;

// The read's linear step of sRGB step `s` (pack/ycbcr.metal srgbStep): `knots` (pack/srgb_knots.h, 513 of them) joined by
// lines, rounded to nearest even
inline uint32_t ycbcrSrgbDecodeStep(const uint16_t *knots, uint32_t s)
{
    const uint32_t i = std::min(s >> 4, 511u), f = s - 16 * i;
    const uint32_t n = 16 * knots[i] + (knots[i + 1] - knots[i]) * f;
    return (n + 7 + ((n >> 4) & 1)) >> 4;
}

// E (pack/srgb_encode.h): the sRGB step a store takes linear step q to, each of 0 .. 8192
inline void ycbcrSrgbEncodeTable(const uint16_t *knots, uint16_t *table)
{
    uint32_t decoded[kYcbcrSrgbSteps + 1];
    for (uint32_t s = 0; s <= kYcbcrSrgbSteps; s++)
        decoded[s] = ycbcrSrgbDecodeStep(knots, s);
    table[0] = 0;
    uint32_t first = 0;  // the first step decoded at or above q
    for (uint32_t q = 1; q <= kYcbcrSrgbSteps; q++) {
        while (decoded[first] < q)
            first++;
        uint32_t end = first;  // past the last step decoded at q
        while (end <= kYcbcrSrgbSteps && decoded[end] == q)
            end++;
        uint32_t lo = first, hi = end - 1;
        if (end == first) {  // no step decodes to q: the nearer neighbour, both when they are as near
            const uint32_t below = q - decoded[first - 1], above = decoded[first] - q;
            lo = below <= above ? first - 1 : first;
            hi = above <= below ? first : first - 1;
        }
        table[q] = uint16_t((lo + hi + 1) / 2);
    }
}

struct YcbcrConvertedTarget {
    uint32_t bits = 8;
    bool srgb = false, full = false;
    YcbcrMatrix matrix = YcbcrMatrix::Bt601;
    bool across = true, down = true;  // the chroma halved
};

// m2v's 15-bit rows (bind/ycbcr_write.rs rows_q15) of luma, Cb and Cr, red, green and blue, by matrix
constexpr int32_t kYcbcrRowsQ15[3][3][3] = {{{9797, 19235, 3736}, {-5537, -10846, 16383}, {16384, -13730, -2654}},
                                            {{6967, 23436, 2365}, {-3756, -12629, 16384}, {16384, -14884, -1501}},
                                            {{8608, 22217, 1943}, {-4574, -11810, 16384}, {16384, -15067, -1318}}};

// and its video-range rows (rows_video), each with the bits it is counted at (10-bit codes: two fewer)
constexpr int32_t kYcbcrRowsVideo[3][3][4] = {
    {{8381, 16455, 3196, 18}, {-4845, -9491, 14336, 18}, {7168, -6007, -1161, 17}},
    {{5960, 20049, 2023, 18}, {-1643, -5525, 7168, 17}, {14336, -13023, -1313, 18}},
    {{3682, 9503, 831, 17}, {-4003, -10334, 14336, 18}, {14336, -13183, -1153, 18}}};

// Each code's row, counted at 2^26, and offset
inline void ycbcrConvertedRows(const YcbcrConvertedTarget &t, int64_t rows[3][3], int64_t offsets[3])
{
    const int i = int(t.matrix), ten = t.bits == 10 ? 2 : 0;
    for (int code = 0; code < 3; code++) {
        offsets[code] = code ? 1 << (t.bits - 1) : t.full ? 0 : 16 << (t.bits - 8);
        const int64_t scale = code ? 1 << t.bits : (1 << t.bits) - 1;
        for (int c = 0; c < 3; c++)
            rows[code][c] =
                t.full ? kYcbcrRowsQ15[i][code][c] * scale
                       : kYcbcrRowsVideo[i][code][c] * (int64_t(1) << (26 - kYcbcrRowsVideo[i][code][3] + ten));
    }
}

// Metal's codes of a pass's store of `rgba` into a converted or sRGB target (`encode` E, for an sRGB one): as ycbcrStore lays
// them out, chroma of the target's subsampling
inline bool ycbcrStoreConverted(const YcbcrConvertedTarget &t, const uint16_t *encode, const float *rgba,
                                uint32_t width, uint32_t height, YcbcrTexelSource source, uint16_t *luma,
                                uint16_t *chroma)
{
    const uint32_t w = t.across ? 2 : 1, h = t.down ? 2 : 1, k = (w / 2) + (h / 2);
    if (!width || !height || width % w || height % h)
        return false;
    int64_t rows[3][3], offsets[3];
    ycbcrConvertedRows(t, rows, offsets);
    const int termBits = t.bits == 10 ? 4 : 6, shift = (t.srgb ? 22 : 20) + int(t.bits) - 8;
    const int64_t max = (1 << t.bits) - 1;
    auto step = [&](float v) {
        const double held = ycbcrHeld(v, source, t.bits);
        return t.srgb ? int64_t(encode[size_t(std::nearbyint(held * kYcbcrSrgbSteps))])
                      : int64_t(std::nearbyint(held * 2048.0));
    };
    auto term = [&](int code, uint32_t texel) {
        int64_t sum = 0;
        for (int c = 0; c < 3; c++)
            sum += rows[code][c] * step(rgba[4 * texel + c]);
        return sum >> shift;
    };
    auto encoded = [&](int code, int64_t sum, uint32_t log2n) {
        const int s = termBits + int(log2n);
        return uint16_t(std::clamp<int64_t>(offsets[code] + ((sum + (int64_t(1) << (s - 1))) >> s), 0, max));
    };
    for (uint32_t i = 0; i < width * height; i++)
        luma[i] = encoded(0, term(0, i), 0);
    const uint32_t cw = width / w, ch = height / h;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++)
            for (int code = 1; code < 3; code++) {
                int64_t sum = 0;
                for (uint32_t dy = 0; dy < h; dy++)
                    for (uint32_t dx = 0; dx < w; dx++)
                        sum += term(code, (y * h + dy) * width + x * w + dx);
                chroma[2 * (y * cw + x) + code - 1] = encoded(code, sum, k);
            }
    return true;
}

}  // namespace raw
