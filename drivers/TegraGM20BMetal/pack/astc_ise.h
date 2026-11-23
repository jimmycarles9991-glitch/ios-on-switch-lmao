// SPDX-License-Identifier: MIT
// ASTC's integer sequences and their unquantization (Khronos Data Format Specification 1.3, 23.12 and 23.13 and 23.17),
// computed rather than looked up: the trit and quint unpacking and the unquantization formulas Mesa's tables are built
// from (src/util/texcompress_astc_luts.cpp, Hans-Kristian Arntzen, MIT).
#pragma once
#include "decode_bits.h"

namespace gm20b {
namespace decode {

struct Quant {
    int bits, trits, quints;
};

static inline int tritOf(uint32_t T, int k)
{
    uint32_t C, t3, t4;
    if (((T >> 2) & 7) == 7) {
        C = (((T >> 5) & 7) << 2) | (T & 3);
        t4 = t3 = 2;
    } else {
        C = T & 0x1f;
        if (((T >> 5) & 3) == 3) {
            t4 = 2;
            t3 = (T >> 7) & 1;
        } else {
            t4 = (T >> 7) & 1;
            t3 = (T >> 5) & 3;
        }
    }
    uint32_t t0, t1, t2;
    if ((C & 3) == 3) {
        t2 = 2;
        t1 = (C >> 4) & 1;
        t0 = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1));
    } else if (((C >> 2) & 3) == 3) {
        t2 = 2;
        t1 = 2;
        t0 = C & 3;
    } else {
        t2 = (C >> 4) & 1;
        t1 = (C >> 2) & 3;
        t0 = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1));
    }
    return int(k == 0 ? t0 : k == 1 ? t1 : k == 2 ? t2 : k == 3 ? t3 : t4);
}

static inline int quintOf(uint32_t Q, int k)
{
    uint32_t q0, q1, q2;
    if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
        q2 = ((Q & 1) << 2) | ((((Q >> 4) & 1) & ~(Q & 1)) << 1) | (((Q >> 3) & 1) & ~(Q & 1));
        q1 = q0 = 4;
    } else {
        uint32_t C;
        if (((Q >> 1) & 3) == 3) {
            q2 = 4;
            C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1);
        } else {
            q2 = (Q >> 5) & 3;
            C = Q & 0x1f;
        }
        if ((C & 7) == 5) {
            q1 = 4;
            q0 = (C >> 3) & 3;
        } else {
            q1 = (C >> 3) & 3;
            q0 = C & 7;
        }
    }
    return int(k == 0 ? q0 : k == 1 ? q1 : q2);
}

// Value `index` of the sequence of `q` integers from bit `start` of `b`
static inline int iseValue(Words4 b, int start, int index, Quant q)
{
    if (q.trits) {
        const int group = index / 5, k = index - group * 5;
        start += group * (5 * q.bits + 8);
        const uint32_t T = field(b, start + q.bits, 2) | field(b, start + 2 * q.bits + 2, 2) << 2 |
                           field(b, start + 3 * q.bits + 4, 1) << 4 | field(b, start + 4 * q.bits + 5, 2) << 5 |
                           field(b, start + 5 * q.bits + 7, 1) << 7;
        const int m = int(field(b, start + k * q.bits + (k * 8 + 4) / 5, q.bits));
        return tritOf(T, k) << q.bits | m;
    }
    if (q.quints) {
        const int group = index / 3, k = index - group * 3;
        start += group * (3 * q.bits + 7);
        const uint32_t Q = field(b, start + q.bits, 3) | field(b, start + 2 * q.bits + 3, 2) << 3 |
                           field(b, start + 3 * q.bits + 5, 2) << 5;
        const int m = int(field(b, start + k * q.bits + (k * 7 + 2) / 3, q.bits));
        return quintOf(Q, k) << q.bits | m;
    }
    return int(field(b, start + index * q.bits, q.bits));
}

static inline int iseBits(Quant q, int count)
{
    return q.bits * count + (q.trits * 8 * count + 4) / 5 + (q.quints * 7 * count + 2) / 3;
}

// The weight range of a block mode's R and H bits (index R | H << 3); bits 0 and no trit or quint: reserved
static inline Quant weightQuant(int index)
{
    switch (index) {
    case 2:
        return {1, 0, 0};
    case 3:
        return {0, 1, 0};
    case 4:
        return {2, 0, 0};
    case 5:
        return {0, 0, 1};
    case 6:
        return {1, 1, 0};
    case 7:
        return {3, 0, 0};
    case 10:
        return {1, 0, 1};
    case 11:
        return {2, 1, 0};
    case 12:
        return {4, 0, 0};
    case 13:
        return {2, 0, 1};
    case 14:
        return {3, 1, 0};
    case 15:
        return {5, 0, 0};
    default:
        return {0, 0, 0};
    }
}

static inline bool reservedQuant(Quant q)
{
    return !q.bits && !q.trits && !q.quints;
}

// A weight in 0..64
static inline int unquantizeWeight(int v, Quant q)
{
    int u;
    if (!q.trits && !q.quints) {
        u = q.bits == 1   ? v * 63
            : q.bits == 2 ? v * 0x15
            : q.bits == 3 ? v * 9
            : q.bits == 4 ? (v << 2) | (v >> 2)
                          : (v << 1) | (v >> 4);
    } else if (!q.bits) {
        return q.trits ? 32 * v : 16 * v;
    } else {
        const int b = (v >> 1) & 1, c = (v >> 2) & 1;
        const int A = 0x7f * (v & 1), D = v >> q.bits;
        int B = 0, C;
        if (q.trits) {
            C = q.bits == 1 ? 50 : q.bits == 2 ? 23 : 11;
            B = q.bits == 2 ? 0x45 * b : q.bits == 3 ? 0x21 * b + 0x42 * c : 0;
        } else {
            C = q.bits == 1 ? 28 : 13;
            B = q.bits == 2 ? 0x42 * b : 0;
        }
        u = (A & 0x20) | (((D * C + B) ^ A) >> 2);
    }
    return u > 32 ? u + 1 : u;
}

// The colour endpoint ranges from the widest, the first whose values fit taken
SWITCH_DECODE_TABLE uint8_t kEndpointQuants[17][3] = {
    {8, 0, 0}, {6, 1, 0}, {5, 0, 1}, {7, 0, 0}, {5, 1, 0}, {4, 0, 1}, {6, 0, 0}, {4, 1, 0}, {3, 0, 1},
    {5, 0, 0}, {3, 1, 0}, {2, 0, 1}, {4, 0, 0}, {2, 1, 0}, {1, 0, 1}, {3, 0, 0}, {1, 1, 0},
};

// The widest range `count` endpoint values fit `bits` in; reserved where none does
static inline Quant endpointQuant(int count, int bits)
{
    for (int i = 0; i < 17; i++) {
        const Quant q = {kEndpointQuants[i][0], kEndpointQuants[i][1], kEndpointQuants[i][2]};
        if (iseBits(q, count) <= bits)
            return q;
    }
    return {0, 0, 0};
}

// An endpoint value in 0..255
static inline int unquantizeEndpoint(int v, Quant q)
{
    if (!q.trits && !q.quints) {
        switch (q.bits) {
        case 1:
            return v * 0xff;
        case 2:
            return v * 0x55;
        case 3:
            return (v << 5) | (v << 2) | (v >> 1);
        case 4:
            return v * 0x11;
        case 5:
            return (v << 3) | (v >> 2);
        case 6:
            return (v << 2) | (v >> 4);
        case 7:
            return (v << 1) | (v >> 6);
        default:
            return v;
        }
    }
    const int b = (v >> 1) & 1, c = (v >> 2) & 1, d = (v >> 3) & 1, e = (v >> 4) & 1, f = (v >> 5) & 1;
    const int D = v >> q.bits, A = (v & 1) * 0x1ff;
    int B = 0, C;
    if (q.trits) {
        C = q.bits == 1 ? 204 : q.bits == 2 ? 93 : q.bits == 3 ? 44 : q.bits == 4 ? 22 : q.bits == 5 ? 11 : 5;
        B = q.bits == 2   ? b * 0x116
            : q.bits == 3 ? b * 0x85 + c * 0x10a
            : q.bits == 4 ? b * 0x41 + c * 0x82 + d * 0x104
            : q.bits == 5 ? b * 0x20 + c * 0x40 + d * 0x81 + e * 0x102
            : q.bits == 6 ? b * 0x10 + c * 0x20 + d * 0x40 + e * 0x80 + f * 0x101
                          : 0;
    } else {
        C = q.bits == 1 ? 113 : q.bits == 2 ? 54 : q.bits == 3 ? 26 : q.bits == 4 ? 13 : 6;
        B = q.bits == 2   ? b * 0x10c
            : q.bits == 3 ? b * 0x82 + c * 0x105
            : q.bits == 4 ? b * 0x40 + c * 0x81 + d * 0x102
            : q.bits == 5 ? b * 0x20 + c * 0x40 + d * 0x80 + e * 0x101
                          : 0;
    }
    return ((A & 0x80) | (((D * C + B) ^ A) >> 2)) & 0xff;
}

}  // namespace decode
}  // namespace gm20b
