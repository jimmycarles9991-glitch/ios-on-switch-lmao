// SPDX-License-Identifier: MIT
// Part of astc_decode.h: a texel's colour from its block's mode, endpoints and weights.
#pragma once

namespace gm20b {
namespace decode {

// What a texel decodes to: the sRGB LDR formats' 8-bit codes, the linear LDR formats' 12-bit ones, the HDR formats'
// half bits (measured: an M4 Max samples a linear LDR texel as k / 4095, the endpoints expanded by 0x101 and the
// interpolation's top 12 bits kept; an sRGB one as its 8-bit code, decode_unorm8's expansion)
enum : int {
    kAstcUnorm8 = 0,
    kAstcUnorm12 = 1,
    kAstcHalf = 2
};

static inline Texel astcError(int form)
{
    return form == kAstcHalf      ? texelOf(0xffff, 0xffff, 0xffff, 0xffff)
           : form == kAstcUnorm12 ? texelOf(0xfff, 0, 0xfff, 0xfff)
                                  : texelOf(0xff, 0, 0xff, 0xff);
}

// A 16-bit UNORM value as the half ASTC's HDR profile takes it to: 0xffff one, else rounded down, and below the
// smallest normal half zero (measured: 1 to 3 read 0, 4 reads 2^-14)
static inline uint32_t unorm16Half(int c)
{
    if (c == 0xffff)
        return 0x3c00;
    const int msb = msb32(uint32_t(c)), e = msb - 1;
    return e < 1 ? 0u : uint32_t((((c << 10) >> msb) & 0x3ff) | e << 10);
}

// A 16-bit HDR value (an LNS one) as a half, past the largest finite one clamped to it
static inline uint32_t lnsHalf(int c)
{
    const int e = c >> 11, m = c & 0x7ff;
    const int mt = m < 512 ? 3 * m : m >= 1536 ? 5 * m - 2048 : 4 * m - 512;
    const int h = (e << 10) + (mt >> 3);
    return (h & 0x7fff) >= 0x7c00 ? 0x7bff : uint32_t(h);
}

static inline int lerp64(int c0, int c1, int w)
{
    return (c0 * (64 - w) + c1 * w + 32) >> 6;
}

static inline uint32_t channelOf(int e0, int e1, int w, int mode, bool alpha, int form)
{
    const bool lns = mode == kModeHdr || (mode == kModeHdrLdrAlpha && !alpha);
    if (lns)
        return lnsHalf(lerp64(e0 << 4, e1 << 4, w));
    if (form == kAstcHalf)
        return unorm16Half(lerp64(e0 * 0x101, e1 * 0x101, w));
    if (form == kAstcUnorm12)
        return uint32_t(lerp64(e0 * 0x101, e1 * 0x101, w) >> 4);
    return uint32_t(lerp64(e0 << 8 | 0x80, e1 << 8 | 0x80, w) >> 8);
}

static inline Texel astcVoidExtent(Words4 b, int form)
{
    const int minS = int(field(b, 12, 13)), maxS = int(field(b, 25, 13)), minT = int(field(b, 38, 13)),
              maxT = int(field(b, 51, 13));
    const bool allOnes = minS == 0x1fff && maxS == 0x1fff && minT == 0x1fff && maxT == 0x1fff;
    if (field(b, 10, 2) != 3 || (!allOnes && (minS >= maxS || minT >= maxT)))
        return astcError(form);
    const int c[4] = {int(field(b, 64, 16)), int(field(b, 80, 16)), int(field(b, 96, 16)), int(field(b, 112, 16))};
    const bool hdrColour = (b.w0 >> 9) & 1;
    if (form == kAstcHalf)
        return hdrColour ? texelOf(c[0], c[1], c[2], c[3])
                         : texelOf(unorm16Half(c[0]), unorm16Half(c[1]), unorm16Half(c[2]), unorm16Half(c[3]));
    if (hdrColour)
        return astcError(form);
    const int shift = form == kAstcUnorm12 ? 4 : 8;
    return texelOf(c[0] >> shift, c[1] >> shift, c[2] >> shift, c[3] >> shift);
}

// Texel (x, y) of a blockW x blockH block in `form`
static inline Texel astcTexel(Words4 b, int blockW, int blockH, int x, int y, int form)
{
    const AstcMode m = astcMode(b, blockW, blockH);
    if (m.voidExtent)
        return astcVoidExtent(b, form);
    const Quant wq = weightQuant(m.weightRange);
    const int planes = m.dual ? 2 : 1, weights = m.gridW * m.gridH * planes, weightBits = iseBits(wq, weights);
    if (m.error || reservedQuant(wq) || weights > 64 || weightBits < 24 || weightBits > 96)
        return astcError(form);
    int cem = m.cem, base = 0, pairs;
    const bool shared = (cem & 3) == 0;
    const int extraCem = m.partitions > 1 && !shared ? 3 * m.partitions - 4 : 0;
    if (m.partitions == 1) {
        pairs = (cem >> 2) + 1;
    } else if (shared) {
        pairs = ((cem >> 4) + 1) * m.partitions;
        const int part = astcPartition(m.seed, x, y, m.partitions, blockW * blockH < 31);
        cem >>= 2;
        base = 2 * ((cem >> 2) + 1) * part;
    } else {
        pairs = (cem & 3) * m.partitions + popcount32(uint32_t(cem >> 2) & ((1u << m.partitions) - 1u));
        const int part = astcPartition(m.seed, x, y, m.partitions, blockW * blockH < 31);
        base = 2 * ((cem & 3) * part + popcount32(uint32_t(cem >> 2) & ((1u << part) - 1u)));
        const int all = int(field(b, 128 - weightBits - extraCem, extraCem)) << 4 | (cem >> 2);
        cem = 4 * ((m.cem & 3) - 1 + ((all >> part) & 1)) + ((all >> (m.partitions + 2 * part)) & 3);
    }
    const int available = maxInt(128 - m.configBits - weightBits, 0);
    const Quant eq = endpointQuant(2 * pairs, available);
    if (pairs > 9 || reservedQuant(eq))
        return astcError(form);
    const Words4 ends = lowBits(b, m.primaryBits + iseBits(eq, 2 * pairs));
    const int count = 2 * ((cem >> 2) + 1);
    int v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 8; i++)
        if (i < count)
            v[i] = unquantizeEndpoint(iseValue(ends, m.primaryBits, base + i, eq), eq);
    const Values8 values = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]};
    const Endpoints e = endpointsOfMode(cem, values);
    if (form != kAstcHalf && e.mode != kModeLdr)
        return astcError(form);
    const Words4 reversed = reversedLow(b, weightBits);
    const int w0 = astcWeight(reversed, m, wq, blockW, blockH, x, y, 0, planes);
    const int w1 = m.dual ? astcWeight(reversed, m, wq, blockW, blockH, x, y, 1, planes) : w0;
    const int ccs = m.dual ? int(field(b, 126 - weightBits - extraCem, 2)) : -1;
    return texelOf(channelOf(e.e0.r, e.e1.r, ccs == 0 ? w1 : w0, e.mode, false, form),
                   channelOf(e.e0.g, e.e1.g, ccs == 1 ? w1 : w0, e.mode, false, form),
                   channelOf(e.e0.b, e.e1.b, ccs == 2 ? w1 : w0, e.mode, false, form),
                   channelOf(e.e0.a, e.e1.a, ccs == 3 ? w1 : w0, e.mode, true, form));
}

}  // namespace decode
}  // namespace gm20b
