// SPDX-License-Identifier: MIT
// ASTC's colour endpoint modes (Khronos Data Format Specification 1.3, 23.14 and 23.15), each pair of endpoints from
// its unquantized values: the LDR modes in 0..255, the HDR modes as 12-bit values (alpha 0x780 where the mode holds
// none). After Mesa's astc_decoder.glsl (Hans-Kristian Arntzen, MIT).
#pragma once
#include "decode_bits.h"

namespace gm20b {
namespace decode {

enum : int {
    kModeLdr = 0,
    kModeHdr = 1,
    kModeHdrLdrAlpha = 2
};

struct Endpoints {
    Int4 e0, e1;
    int mode;
};

struct Values8 {
    int v0, v1, v2, v3, v4, v5, v6, v7;
};

static inline Int4 blueContract(int r, int g, int b, int a)
{
    return int4Of((r + b) >> 1, (g + b) >> 1, b, a);
}

static inline Endpoints endpointsOf(Int4 e0, Int4 e1, int mode)
{
    Endpoints e = {e0, e1, mode};
    return e;
}

// The spec's bit_transfer_signed of (a, b): b takes a's top bit; a becomes the signed 6-bit offset below it
static inline int transferredBase(int a, int b)
{
    return (b >> 1) | (a & 0x80);
}

static inline int transferredOffset(int a)
{
    const int six = (a >> 1) & 0x3f;
    return six & 0x20 ? six - 0x40 : six;
}

static inline Endpoints hdrLumaSmallRange(int v0, int v1)
{
    int y0, d;
    if (v0 & 0x80) {
        y0 = ((v1 & 0xe0) << 4) | ((v0 & 0x7f) << 2);
        d = (v1 & 0x1f) << 2;
    } else {
        y0 = ((v1 & 0xf0) << 4) | ((v0 & 0x7f) << 1);
        d = (v1 & 0x0f) << 1;
    }
    const int y1 = minInt(y0 + d, 0xfff);
    return endpointsOf(int4Of(y0, y0, y0, 0x780), int4Of(y1, y1, y1, 0x780), kModeHdr);
}

static inline Endpoints hdrRgbScale(int v0, int v1, int v2, int v3)
{
    const int modeValue = ((v0 & 0xc0) >> 6) | ((v1 & 0x80) >> 5) | ((v2 & 0x80) >> 4);
    int major, mode;
    if ((modeValue & 0xc) != 0xc) {
        major = modeValue >> 2;
        mode = modeValue & 3;
    } else if (modeValue != 0xf) {
        major = modeValue & 3;
        mode = 4;
    } else {
        major = 0;
        mode = 5;
    }
    int red = v0 & 0x3f, green = v1 & 0x1f, blue = v2 & 0x1f, scale = v3 & 0x1f;
    const int x0 = (v1 >> 6) & 1, x1 = (v1 >> 5) & 1, x2 = (v2 >> 6) & 1, x3 = (v2 >> 5) & 1, x4 = (v3 >> 7) & 1,
              x5 = (v3 >> 6) & 1, x6 = (v3 >> 5) & 1;
    const int ohm = 1 << mode;
    green |= (ohm & 0x30 ? x0 << 6 : 0) | (ohm & 0x3a ? x1 << 5 : 0);
    blue |= (ohm & 0x30 ? x2 << 6 : 0) | (ohm & 0x3a ? x3 << 5 : 0);
    scale |= (ohm & 0x3d ? x6 << 5 : 0) | (ohm & 0x2d ? x5 << 6 : 0) | (ohm & 0x04 ? x4 << 7 : 0);
    red |= (ohm & 0x3b ? x4 << 6 : 0) | (ohm & 0x04 ? x3 << 6 : 0) | (ohm & 0x10 ? x5 << 7 : 0) |
           (ohm & 0x0f ? x2 << 7 : 0) | (ohm & 0x05 ? x1 << 8 : 0) | (ohm & 0x0a ? x0 << 8 : 0) |
           (ohm & 0x05 ? x0 << 9 : 0) | (ohm & 0x02 ? x6 << 9 : 0) | (ohm & 0x01 ? x3 << 10 : 0) |
           (ohm & 0x02 ? x5 << 10 : 0);
    const int shift = maxInt(mode, 1);
    red <<= shift;
    green <<= shift;
    blue <<= shift;
    scale <<= shift;
    if (mode != 5) {
        green = red - green;
        blue = red - blue;
    }
    int c0 = red, c1 = green, c2 = blue;
    if (major == 1) {
        c0 = green;
        c1 = red;
    } else if (major == 2) {
        c0 = blue;
        c2 = red;
    }
    return endpointsOf(
        int4Of(clampInt(c0 - scale, 0, 0xfff), clampInt(c1 - scale, 0, 0xfff), clampInt(c2 - scale, 0, 0xfff), 0x780),
        int4Of(clampInt(c0, 0, 0xfff), clampInt(c1, 0, 0xfff), clampInt(c2, 0, 0xfff), 0x780), kModeHdr);
}

static inline Endpoints hdrRgbDirect(int v0, int v1, int v2, int v3, int v4, int v5)
{
    const int major = ((v4 & 0x80) >> 7) | ((v5 & 0x80) >> 6);
    if (major == 3)
        return endpointsOf(int4Of(v0 << 4, v2 << 4, (v4 & 0x7f) << 5, 0x780),
                           int4Of(v1 << 4, v3 << 4, (v5 & 0x7f) << 5, 0x780), kModeHdr);
    const int mode = ((v1 & 0x80) >> 7) | ((v2 & 0x80) >> 6) | ((v3 & 0x80) >> 5);
    int va = v0 | ((v1 & 0x40) << 2), vb0 = v2 & 0x3f, vb1 = v3 & 0x3f, vc = v1 & 0x3f;
    int dBits = 7 - (mode & 1);
    if ((mode & 5) == 4)
        dBits -= 2;
    int vd0 = (v4 & 0x7f) & ((1 << dBits) - 1), vd1 = (v5 & 0x7f) & ((1 << dBits) - 1);
    vd0 = vd0 & (1 << (dBits - 1)) ? vd0 - (1 << dBits) : vd0;
    vd1 = vd1 & (1 << (dBits - 1)) ? vd1 - (1 << dBits) : vd1;
    const int x0 = (v2 >> 6) & 1, x1 = (v3 >> 6) & 1, x2 = (v4 >> 6) & 1, x3 = (v5 >> 6) & 1, x4 = (v4 >> 5) & 1,
              x5 = (v5 >> 5) & 1;
    const int ohm = 1 << mode;
    va |= (ohm & 0xa4 ? x0 << 9 : 0) | (ohm & 0x08 ? x2 << 9 : 0) | (ohm & 0x50 ? x4 << 9 : 0) |
          (ohm & 0x50 ? x5 << 10 : 0) | (ohm & 0xa0 ? x1 << 10 : 0) | (ohm & 0xc0 ? x2 << 11 : 0);
    vc |= (ohm & 0x04 ? x1 << 6 : 0) | (ohm & 0xe8 ? x3 << 6 : 0) | (ohm & 0x20 ? x2 << 7 : 0);
    vb0 |= (ohm & 0x5b ? x0 << 6 : 0) | (ohm & 0x12 ? x2 << 7 : 0);
    vb1 |= (ohm & 0x5b ? x1 << 6 : 0) | (ohm & 0x12 ? x3 << 7 : 0);
    const int shift = (mode >> 1) ^ 3;
    va <<= shift;
    vb0 <<= shift;
    vb1 <<= shift;
    vc <<= shift;
    // vd0 and vd1 are signed, and C++ before 20 leaves a left shift of a negative value undefined: a multiply
    vd0 *= 1 << shift;
    vd1 *= 1 << shift;
    Int4 e1 = int4Of(clampInt(va, 0, 0xfff), clampInt(va - vb0, 0, 0xfff), clampInt(va - vb1, 0, 0xfff), 0x780);
    Int4 e0 = int4Of(clampInt(va - vc, 0, 0xfff), clampInt(va - vb0 - vc - vd0, 0, 0xfff),
                     clampInt(va - vb1 - vc - vd1, 0, 0xfff), 0x780);
    if (major == 1) {
        e0 = int4Of(e0.g, e0.r, e0.b, e0.a);
        e1 = int4Of(e1.g, e1.r, e1.b, e1.a);
    } else if (major == 2) {
        e0 = int4Of(e0.b, e0.g, e0.r, e0.a);
        e1 = int4Of(e1.b, e1.g, e1.r, e1.a);
    }
    return endpointsOf(e0, e1, kModeHdr);
}

// The two alpha endpoints of mode 15 (e0 in .r, e1 in .g)
static inline Int4 hdrAlpha(int v6, int v7)
{
    const int mode = ((v6 >> 7) & 1) | ((v7 >> 6) & 2);
    v6 &= 0x7f;
    v7 &= 0x7f;
    if (mode == 3)
        return int4Of(v6 << 5, v7 << 5, 0, 0);
    v6 |= (v7 << (mode + 1)) & 0x780;
    v7 &= 0x3f >> mode;
    v7 ^= 0x20 >> mode;
    v7 -= 0x20 >> mode;
    v6 <<= 4 - mode;
    v7 *= 1 << (4 - mode);  // signed: a multiply, as hdrRgbDirect's vd0 and vd1
    return int4Of(v6, clampInt(v7 + v6, 0, 0xfff), 0, 0);
}

static inline Endpoints rgbBaseOffset(Values8 v, bool alpha)
{
    const int b0 = transferredBase(v.v1, v.v0), b1 = transferredBase(v.v3, v.v2), b2 = transferredBase(v.v5, v.v4);
    const int o0 = transferredOffset(v.v1), o1 = transferredOffset(v.v3), o2 = transferredOffset(v.v5);
    const int b3 = alpha ? transferredBase(v.v7, v.v6) : 0xff, o3 = alpha ? transferredOffset(v.v7) : 0;
    Int4 e0, e1;
    if (o0 + o1 + o2 >= 0) {
        e0 = int4Of(b0, b1, b2, b3);
        e1 = int4Of(b0 + o0, b1 + o1, b2 + o2, b3 + o3);
    } else {
        e0 = blueContract(b0 + o0, b1 + o1, b2 + o2, b3 + o3);
        e1 = blueContract(b0, b1, b2, b3);
    }
    e0 = int4Of(clampInt(e0.r, 0, 0xff), clampInt(e0.g, 0, 0xff), clampInt(e0.b, 0, 0xff), clampInt(e0.a, 0, 0xff));
    e1 = int4Of(clampInt(e1.r, 0, 0xff), clampInt(e1.g, 0, 0xff), clampInt(e1.b, 0, 0xff), clampInt(e1.a, 0, 0xff));
    return endpointsOf(e0, e1, kModeLdr);
}

static inline Endpoints rgbDirect(Values8 v, bool alpha)
{
    const int a0 = alpha ? v.v6 : 0xff, a1 = alpha ? v.v7 : 0xff;
    if (v.v1 + v.v3 + v.v5 >= v.v0 + v.v2 + v.v4)
        return endpointsOf(int4Of(v.v0, v.v2, v.v4, a0), int4Of(v.v1, v.v3, v.v5, a1), kModeLdr);
    return endpointsOf(blueContract(v.v1, v.v3, v.v5, a1), blueContract(v.v0, v.v2, v.v4, a0), kModeLdr);
}

// The endpoints of colour endpoint mode `cem` from its values
static inline Endpoints endpointsOfMode(int cem, Values8 v)
{
    switch (cem) {
    case 0:
        return endpointsOf(int4Of(v.v0, v.v0, v.v0, 0xff), int4Of(v.v1, v.v1, v.v1, 0xff), kModeLdr);
    case 1: {
        const int l0 = (v.v0 >> 2) | (v.v1 & 0xc0), l1 = minInt(l0 + (v.v1 & 0x3f), 0xff);
        return endpointsOf(int4Of(l0, l0, l0, 0xff), int4Of(l1, l1, l1, 0xff), kModeLdr);
    }
    case 2: {
        const int y0 = v.v1 >= v.v0 ? v.v0 << 4 : (v.v1 << 4) + 8, y1 = v.v1 >= v.v0 ? v.v1 << 4 : (v.v0 << 4) - 8;
        return endpointsOf(int4Of(y0, y0, y0, 0x780), int4Of(y1, y1, y1, 0x780), kModeHdr);
    }
    case 3:
        return hdrLumaSmallRange(v.v0, v.v1);
    case 4:
        return endpointsOf(int4Of(v.v0, v.v0, v.v0, v.v2), int4Of(v.v1, v.v1, v.v1, v.v3), kModeLdr);
    case 5: {
        const int l = transferredBase(v.v1, v.v0), dl = transferredOffset(v.v1);
        const int a = transferredBase(v.v3, v.v2), da = transferredOffset(v.v3);
        const int l0 = clampInt(l, 0, 0xff), a0 = clampInt(a, 0, 0xff);
        const int l1 = clampInt(l + dl, 0, 0xff), a1 = clampInt(a + da, 0, 0xff);
        return endpointsOf(int4Of(l0, l0, l0, a0), int4Of(l1, l1, l1, a1), kModeLdr);
    }
    case 6:
        return endpointsOf(int4Of((v.v0 * v.v3) >> 8, (v.v1 * v.v3) >> 8, (v.v2 * v.v3) >> 8, 0xff),
                           int4Of(v.v0, v.v1, v.v2, 0xff), kModeLdr);
    case 7:
        return hdrRgbScale(v.v0, v.v1, v.v2, v.v3);
    case 8:
        return rgbDirect(v, false);
    case 9:
        return rgbBaseOffset(v, false);
    case 10:
        return endpointsOf(int4Of((v.v0 * v.v3) >> 8, (v.v1 * v.v3) >> 8, (v.v2 * v.v3) >> 8, v.v4),
                           int4Of(v.v0, v.v1, v.v2, v.v5), kModeLdr);
    case 12:
        return rgbDirect(v, true);
    case 13:
        return rgbBaseOffset(v, true);
    default: {
        Endpoints e = hdrRgbDirect(v.v0, v.v1, v.v2, v.v3, v.v4, v.v5);
        if (cem == 14) {
            e.e0.a = v.v6;
            e.e1.a = v.v7;
            e.mode = kModeHdrLdrAlpha;
        } else if (cem == 15) {
            const Int4 a = hdrAlpha(v.v6, v.v7);
            e.e0.a = a.r;
            e.e1.a = a.g;
        }
        return e;
    }
    }
}

}  // namespace decode
}  // namespace gm20b
