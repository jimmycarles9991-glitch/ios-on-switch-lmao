// SPDX-License-Identifier: MIT
// third_party/mesa/src/nouveau/compiler/nak_nir_algebraic.py: metal2vulkan's half rounding on a device without Float16
// (fquantize2f16, and where that gave zero the float rounded to the half denormal grid, 2^-24) is the round trip through
// f16 with round-to-nearest-even keeping denormals, for every f32 input, so NAK may emit it as two conversions; two
// halves added or multiplied in f32 and rounded back to half, a sum's rounding being fquantize2f16 or the sum itself
// where that gave zero, are the half sum or product (this Mac's f16 arithmetic, denormals kept), for every pair, so NAK
// may emit SM53's HADD2 and HMUL2 for them; and metal2vulkan's integer widening of a half and narrowing of an f32 holding
// one are the conversions, so NAK may emit one F2F for each. fquantize2f16 widens on SM53's half ALU, exactly under the
// device's measured HADD2.F32 (nak/from_nir.rs).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

float asFloat(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

uint32_t bitsOf(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// NIR's fquantize2f16 (nir_opcodes.py)
float quantize(float x)
{
    return std::fabs(x) < 0x1p-14f ? std::copysign(0.0f, x) : float(_Float16(x));
}

float metal2vulkan(float x)
{
    const float q = quantize(x);
    return q == 0.0f ? std::nearbyint(x * 0x1p24f) * 0x1p-24f : q;
}

float roundTrip(float x)
{
    return float(_Float16(x));
}

TEST(NakHalfRound, Metal2vulkansHalfRoundingIsTheF16RoundTripForEveryFloat)
{
    uint64_t differ = 0;
    for (uint64_t i = 0; i <= 0xffffffffull; i++) {
        const float x = asFloat(uint32_t(i)), a = metal2vulkan(x), b = roundTrip(x);
        if (!(std::isnan(a) && std::isnan(b)) && bitsOf(a) != bitsOf(b))
            differ++;
    }
    EXPECT_EQ(differ, 0u);
}

uint16_t halfBits(_Float16 x)
{
    uint16_t u;
    memcpy(&u, &x, 2);
    return u;
}

_Float16 asHalf(uint16_t u)
{
    _Float16 x;
    memcpy(&x, &u, 2);
    return x;
}

// The CPU's own f16 instructions, so the compiler cannot evaluate the half arithmetic in float
_Float16 nativeAdd(_Float16 a, _Float16 b)
{
    _Float16 r;
    __asm__("fadd %h0, %h1, %h2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

_Float16 nativeMul(_Float16 a, _Float16 b)
{
    _Float16 r;
    __asm__("fmul %h0, %h1, %h2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

_Float16 narrow(float f)
{
    _Float16 r;
    __asm__("fcvt %h0, %s1" : "=w"(r) : "w"(f));
    return r;
}

float widen(_Float16 a)
{
    float r;
    __asm__("fcvt %s0, %h1" : "=w"(r) : "w"(a));
    return r;
}

bool halfNan(uint16_t u)
{
    return (u & 0x7c00) == 0x7c00 && (u & 0x3ff);
}

TEST(NakHalfRound, HalfSumsAndProductsInFloatRoundToTheHalfOnes)
{
    uint64_t sums = 0, sumForms = 0, products = 0;
    for (uint32_t i = 0; i < 65536; i++)
        for (uint32_t j = 0; j < 65536; j++) {
            const _Float16 a = asHalf(uint16_t(i)), b = asHalf(uint16_t(j));
            const float x = widen(a), y = widen(b);
            uint16_t viaFloat = halfBits(narrow(x + y)), native = halfBits(nativeAdd(a, b));
            sums += viaFloat != native && !(halfNan(viaFloat) && halfNan(native));
            const float s = x + y, q = quantize(s), kept = q == 0.0f ? s : q;
            sumForms += bitsOf(kept) != bitsOf(widen(nativeAdd(a, b))) && !(std::isnan(kept) && halfNan(native));
            viaFloat = halfBits(narrow(x * y));
            native = halfBits(nativeMul(a, b));
            products += viaFloat != native && !(halfNan(viaFloat) && halfNan(native));
        }
    EXPECT_EQ(sums, 0u);
    EXPECT_EQ(sumForms, 0u);
    EXPECT_EQ(products, 0u);
}

bool f32Nan(uint32_t u)
{
    return (u & 0x7f800000) == 0x7f800000 && (u & 0x7fffff);
}

// GM20B's HADD2.F32 of a half and -0, as gpu.sm53_probe measured the half ALU with an f32 result: the sum rounded to
// half, a half denormal given as a zero of its sign
float deviceWidening(_Float16 h)
{
    const uint16_t u = halfBits(h);
    if ((u & 0x7c00) == 0 && (u & 0x3ff))
        return std::copysign(0.0f, widen(h));
    return widen(h);
}

// nak/from_nir.rs fquantize2f16 before Volta, its widening on the half ALU: whether F2F.ftz keeps a half denormal or
// flushes it, the select takes a zero wherever HADD2.F32 would give one, so the sequence is NIR's fquantize2f16 for
// every f32
TEST(NakHalfRound, QuantizeWidensOnTheHalfAluForEveryFloat)
{
    uint64_t differ = 0;
    for (uint64_t i = 0; i <= 0xffffffffull; i++) {
        const float x = asFloat(uint32_t(i)), want = quantize(x);
        const _Float16 kept = narrow(x);
        const uint16_t keptBits = halfBits(kept);
        const _Float16 flushed = (keptBits & 0x7c00) ? kept : asHalf(keptBits & 0x8000);
        for (_Float16 h : {kept, flushed}) {
            const float got = std::fabs(x) < 0x1p-14f ? asFloat(uint32_t(i) & 0x80000000u) : deviceWidening(h);
            differ += bitsOf(got) != bitsOf(want) && !(std::isnan(got) && std::isnan(want));
        }
    }
    EXPECT_EQ(differ, 0u);
}

// narrow.rs half_bits_to_f32_bits as NIR has it: the low 16 bits of h, the rest ignored
uint32_t metal2vulkanWidened(uint32_t h)
{
    const uint32_t em = h & 0x7fff;
    const uint32_t normal = (em << 13) + (em >= 0x7c00 ? 0x70000000u : 0x38000000u);
    const uint32_t denormal = bitsOf(float(em) * 0x1p-24f);
    return ((h & 0x8000) << 16) | (em < 0x400 ? denormal : normal);
}

// narrow.rs f32_bits_to_half_bits under native half rounding, as NIR has it
uint32_t metal2vulkanBits(uint32_t x)
{
    const uint32_t mag = x & 0x7fffffff;
    const bool special = mag >= 0x7f800000;
    uint32_t normal = (mag >> 13) + (special ? 0xfffc8000u : 0xfffe4000u);
    normal = std::min(normal, special ? 0x23c00u : 0x7c00u);
    if (mag + 0x807fffffu < 0x1fff)
        normal = 0x7e00;
    const bool denormal = mag < 0x38800000;
    const uint32_t scaled = uint32_t(asFloat(denormal ? mag : 0) * 16777216.0f);
    return ((x >> 16) & 0x8000) | (denormal ? scaled : normal);
}

TEST(NakHalfRound, Metal2vulkansHalfWideningIsF2FForEveryBitPattern)
{
    uint32_t differ = 0;
    for (uint32_t h = 0; h < 65536; h++) {
        const uint32_t got = metal2vulkanWidened(h | 0xabcd0000u), want = bitsOf(widen(asHalf(uint16_t(h))));
        differ += got != want && !(f32Nan(got) && f32Nan(want));
    }
    EXPECT_EQ(differ, 0u);
}

TEST(NakHalfRound, Metal2vulkansHalfNarrowingIsF2FForEveryFloatHoldingAHalf)
{
    uint64_t halves = 0, differ = 0;
    for (uint64_t i = 0; i <= 0xffffffffull; i++) {
        const float x = asFloat(uint32_t(i));
        const _Float16 h = narrow(x);
        if (!std::isnan(x) && bitsOf(widen(h)) != uint32_t(i))
            continue;
        halves++;
        const uint16_t got = uint16_t(metal2vulkanBits(uint32_t(i))), want = halfBits(h);
        differ += got != want && !(halfNan(got) && halfNan(want));
    }
    EXPECT_GT(halves, 65536u);
    EXPECT_EQ(differ, 0u);
}

}  // namespace
