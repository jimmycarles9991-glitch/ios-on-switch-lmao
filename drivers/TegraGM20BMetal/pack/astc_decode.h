// SPDX-License-Identifier: MIT
// One texel of an ASTC block (Khronos Data Format Specification 1.3, chapter 23) as Apple's GPUs sample it, measured on
// an M4 Max (harness/decodeoracle.mm, astc_texel.h's forms). Any block footprint, Metal's 4x2 and 8x4 among them.
// Block mode, weight infill and partition selection after Mesa's astc_decoder.glsl (Hans-Kristian Arntzen, MIT) and the
// spec's selection function.
#pragma once
#include "astc_endpoints.h"
#include "astc_ise.h"

namespace gm20b {
namespace decode {

struct AstcMode {
    int gridW, gridH, weightRange, partitions, seed, cem, configBits, primaryBits;
    bool dual, voidExtent, error;
};

static inline AstcMode astcMode(Words4 b, int blockW, int blockH)
{
    AstcMode m = {};
    const uint32_t x = b.w0;
    m.voidExtent = (x & 0x1ff) == 0x1fc;
    if (m.voidExtent)
        return m;
    m.dual = (x & (1u << 10)) != 0;
    const uint32_t higher = (x >> 2) & 3, lower = x & 3;
    if (lower) {
        m.weightRange = int(((x >> 4) & 1) | ((x << 1) & 6) | ((x >> 6) & 8));
        if (higher < 2) {
            m.gridW = int(((x >> 7) & 3) + 4 + 4 * higher);
            m.gridH = int(((x >> 5) & 3) + 2);
        } else if (higher == 2) {
            m.gridW = int(((x >> 5) & 3) + 2);
            m.gridH = int(((x >> 7) & 3) + 8);
        } else if (x & (1u << 8)) {
            m.gridW = int(((x >> 7) & 1) + 2);
            m.gridH = int(((x >> 5) & 3) + 2);
        } else {
            m.gridW = int(((x >> 5) & 3) + 2);
            m.gridH = int(((x >> 7) & 1) + 6);
        }
    } else {
        int p3 = int((x >> 9) & 1);
        const int hi = int((x >> 7) & 3), lo = int((x >> 5) & 3);
        if (hi == 0) {
            m.gridW = 12;
            m.gridH = lo + 2;
        } else if (hi == 1) {
            m.gridW = lo + 2;
            m.gridH = 12;
        } else if (hi == 2) {
            m.dual = false;
            p3 = 0;
            m.gridW = lo + 6;
            m.gridH = int(((x >> 9) & 3) + 6);
        } else if (lo == 0) {
            m.gridW = 6;
            m.gridH = 10;
        } else if (lo == 1) {
            m.gridW = 10;
            m.gridH = 6;
        } else {
            m.error = true;
        }
        m.weightRange = int(((x >> 4) & 1) | (((x >> 2) & 1) << 1) | (((x >> 3) & 1) << 2)) | p3 << 3;
    }
    m.partitions = int((x >> 11) & 3) + 1;
    if (m.partitions > 1) {
        m.seed = int((x >> 13) & 0x3ff);
        m.cem = int((x >> 23) & 0x3f);
        m.primaryBits = 29;
        m.configBits = (m.cem & 3) == 0 ? 29 : 25 + 3 * m.partitions;
    } else {
        m.cem = int((x >> 13) & 0xf);
        m.primaryBits = m.configBits = 17;
    }
    if (m.dual)
        m.configBits += 2;
    if (m.gridW > blockW || m.gridH > blockH || (m.dual && m.partitions > 3))
        m.error = true;
    return m;
}

// The spec's partition selection function (23.21) of a 2D block
static inline int astcPartition(int seed, int x, int y, int partitions, bool small)
{
    if (small) {
        x <<= 1;
        y <<= 1;
    }
    seed += (partitions - 1) * 1024;
    uint32_t r = uint32_t(seed);
    r ^= r >> 15;
    r -= r << 17;
    r += r << 7;
    r += r << 4;
    r ^= r >> 5;
    r += r << 16;
    r ^= r >> 7;
    r ^= r >> 3;
    r ^= r << 6;
    r ^= r >> 17;
    int s1 = int(r & 0xf), s2 = int((r >> 4) & 0xf), s3 = int((r >> 8) & 0xf), s4 = int((r >> 12) & 0xf);
    int s5 = int((r >> 16) & 0xf), s6 = int((r >> 20) & 0xf), s7 = int((r >> 24) & 0xf), s8 = int((r >> 28) & 0xf);
    s1 *= s1;
    s2 *= s2;
    s3 *= s3;
    s4 *= s4;
    s5 *= s5;
    s6 *= s6;
    s7 *= s7;
    s8 *= s8;
    int sh1, sh2;
    if (seed & 1) {
        sh1 = seed & 2 ? 4 : 5;
        sh2 = partitions == 3 ? 6 : 5;
    } else {
        sh1 = partitions == 3 ? 6 : 5;
        sh2 = seed & 2 ? 4 : 5;
    }
    const int a = ((s1 >> sh1) * x + (s2 >> sh2) * y + int(r >> 14)) & 0x3f;
    const int b = ((s3 >> sh1) * x + (s4 >> sh2) * y + int(r >> 10)) & 0x3f;
    const int c = partitions < 3 ? 0 : ((s5 >> sh1) * x + (s6 >> sh2) * y + int(r >> 6)) & 0x3f;
    const int d = partitions < 4 ? 0 : ((s7 >> sh1) * x + (s8 >> sh2) * y + int(r >> 2)) & 0x3f;
    return a >= b && a >= c && a >= d ? 0 : b >= c && b >= d ? 1 : c >= d ? 2 : 3;
}

// The infilled weight (0..64) of plane `plane` of `planes` at texel (x, y)
static inline int astcWeight(Words4 reversed, AstcMode m, Quant q, int blockW, int blockH, int x, int y, int plane,
                             int planes)
{
    const int ds = (1024 + blockW / 2) / (blockW - 1), dt = (1024 + blockH / 2) / (blockH - 1);
    const int gs = (ds * x * (m.gridW - 1) + 32) >> 6, gt = (dt * y * (m.gridH - 1) + 32) >> 6;
    const int js = gs >> 4, jt = gt >> 4, fs = gs & 15, ft = gt & 15;
    const int v0 = js + jt * m.gridW;
    const int p00 = unquantizeWeight(iseValue(reversed, 0, v0 * planes + plane, q), q);
    const int p01 = fs ? unquantizeWeight(iseValue(reversed, 0, (v0 + 1) * planes + plane, q), q) : p00;
    const int p10 = ft ? unquantizeWeight(iseValue(reversed, 0, (v0 + m.gridW) * planes + plane, q), q) : p00;
    const int p11 = fs && ft ? unquantizeWeight(iseValue(reversed, 0, (v0 + m.gridW + 1) * planes + plane, q), q)
                             : (fs ? p01 : p10);
    const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
    return (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
}

}  // namespace decode
}  // namespace gm20b

#include "astc_texel.h"
