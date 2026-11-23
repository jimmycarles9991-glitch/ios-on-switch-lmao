// SPDX-License-Identifier: MIT
// The integer helpers decode.metal's decoders (astc_decode.h, pvrtc_decode.h) share with the host: one source compiled
// as Metal for the pack's decode states and as C++ for the host tests and the Mac oracle (harness/decodeoracle.mm).
#pragma once

#if defined(__METAL_VERSION__)
#define SWITCH_DECODE_TABLE constant
#else
#include <cstdint>
#define SWITCH_DECODE_TABLE static const
#endif

namespace gm20b {
namespace decode {

struct Words4 {
    uint32_t w0, w1, w2, w3;
};

struct Texel {
    uint32_t r, g, b, a;
};

struct Int4 {
    int r, g, b, a;
};

static inline Texel texelOf(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    Texel t = {r, g, b, a};
    return t;
}

static inline Int4 int4Of(int r, int g, int b, int a)
{
    Int4 v = {r, g, b, a};
    return v;
}

static inline uint32_t reverse32(uint32_t v)
{
#if defined(__METAL_VERSION__)
    return reverse_bits(v);
#else
    return __builtin_bitreverse32(v);
#endif
}

static inline int popcount32(uint32_t v)
{
#if defined(__METAL_VERSION__)
    return int(popcount(v));
#else
    return __builtin_popcount(v);
#endif
}

// The highest set bit's index, -1 for none
static inline int msb32(uint32_t v)
{
#if defined(__METAL_VERSION__)
    return v ? int(31 - clz(v)) : -1;
#else
    return v ? 31 - __builtin_clz(v) : -1;
#endif
}

static inline int clampInt(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int minInt(int a, int b)
{
    return a < b ? a : b;
}

static inline int maxInt(int a, int b)
{
    return a > b ? a : b;
}

// The 16-bit UNORM code nearest k / 4095 (k 0..4095): k * 65535 / 4095 is 16k + k / 273, never a tie
static inline uint32_t unorm12To16(uint32_t k)
{
    return 16 * k + (k + 136) / 273;
}

// Selects, not an indexed array: a thread array a fragment indexes at run time lives in local memory
static inline uint32_t wordAt(Words4 b, int i)
{
    return i == 0 ? b.w0 : i == 1 ? b.w1 : i == 2 ? b.w2 : b.w3;
}

// Bits [at, at + count) of the 128, count 0..32
static inline uint32_t field(Words4 b, int at, int count)
{
    if (count <= 0)
        return 0;
    const int i = at >> 5, s = at & 31;
    uint32_t v = wordAt(b, i) >> s;
    if (s && s + count > 32)
        v |= wordAt(b, i + 1) << (32 - s);
    return count >= 32 ? v : v & ((1u << count) - 1u);
}

// The 128 bits in reverse order, and only their lowest `count`
static inline Words4 reversedLow(Words4 b, int count)
{
    Words4 r = {reverse32(b.w3), reverse32(b.w2), reverse32(b.w1), reverse32(b.w0)};
    const int n[4] = {count, count - 32, count - 64, count - 96};
    uint32_t m[4];
    for (int k = 0; k < 4; k++)
        m[k] = n[k] >= 32 ? 0xffffffffu : n[k] <= 0 ? 0u : (1u << n[k]) - 1u;
    r.w0 &= m[0];
    r.w1 &= m[1];
    r.w2 &= m[2];
    r.w3 &= m[3];
    return r;
}

// The lowest `count` bits alone
static inline Words4 lowBits(Words4 b, int count)
{
    const int n[4] = {count, count - 32, count - 64, count - 96};
    uint32_t m[4];
    for (int k = 0; k < 4; k++)
        m[k] = n[k] >= 32 ? 0xffffffffu : n[k] <= 0 ? 0u : (1u << n[k]) - 1u;
    b.w0 &= m[0];
    b.w1 &= m[1];
    b.w2 &= m[2];
    b.w3 &= m[3];
    return b;
}

}  // namespace decode
}  // namespace gm20b
