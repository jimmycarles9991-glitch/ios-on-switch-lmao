// SPDX-License-Identifier: MIT
// Rules resolve.twod and resolve.exact (raw/runtime/raw_resolve_rule.h): where they are taken, and the arithmetic they
// rest on, exhaustively, against the ROP's unorm store rule (x1-units UNITS.md): u = floor(v * 2^(n+4)), code =
// (u * (2^n - 1) + 2^(n+3) - 1) >> (n+4); RGBA16F toward zero; the 8-bit blender round((S*Fs + D*Fd) / 255).
// Each candidate for an exact 2D-engine resolve that fails is held to the counter-example it fails on.
#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_resolve_rule.h"

namespace {

// The store rule of an exact value num / den in [0, 1] into an n-bit unorm code
uint32_t store(uint64_t num, uint64_t den, uint32_t n = 8)
{
    const uint64_t u = (num << (n + 4)) / den;
    return uint32_t((u * ((1u << n) - 1) + (1u << (n + 3)) - 1) >> (n + 4));
}

uint32_t metal(uint32_t sum, uint32_t n)
{
    return (sum + n / 2) / n;
}

// The 2D engine's box of n samples summing to `sum`, one rounding of the mean, read directly and through the complement
uint32_t direct(uint32_t sum, uint32_t n)
{
    return store(sum, uint64_t(n) * 255);
}

uint32_t complemented(uint32_t sum, uint32_t n)
{
    return 255 - direct(255 * n - sum, n);
}

// 4x as two rounded 2:1 steps of pair sums p and q, directly and through the complement
uint32_t twoSteps(uint32_t p, uint32_t q)
{
    return direct(direct(p, 2) + direct(q, 2), 2);
}

uint32_t twoStepsComplemented(uint32_t p, uint32_t q)
{
    return 255 - twoSteps(510 - p, 510 - q);
}

double fp16TowardZero(double v)
{
    if (v <= 0)
        return 0;
    int e = 0;
    std::frexp(v, &e);
    const double ulp = std::ldexp(1.0, std::max(e - 11, -24));
    return std::floor(v / ulp) * ulp;
}

uint32_t storeDyadic(double v)
{
    const uint64_t u = uint64_t(std::floor(v * 4096));
    return uint32_t((u * 255 + 2047) >> 12);
}

raw::ResolveAsk screenBound(uint8_t samples)
{
    raw::ResolveAsk a;
    a.format = 80;
    a.samples = samples;
    a.wholeTarget = true;
    return a;
}

TEST(ResolveRule, TakenForScreenBoundUnorm8Only)
{
    raw::ResolveAsk a = screenBound(4);
    EXPECT_TRUE(raw::twodResolveAcceptable(a));
    a.samples = 2;
    EXPECT_TRUE(raw::twodResolveAcceptable(a));
    a.format = 81;
    EXPECT_FALSE(raw::twodResolveAcceptable(a));
    a.format = 70;
    a.readable = true;
    EXPECT_FALSE(raw::twodResolveAcceptable(a));
    a.readable = false;
    a.wholeTarget = false;
    EXPECT_FALSE(raw::twodResolveAcceptable(a));
    a.wholeTarget = true;
    a.samples = 8;
    EXPECT_FALSE(raw::twodResolveAcceptable(a));
    a.samples = 4;
    a.shadowTarget = true;
    EXPECT_FALSE(raw::twodResolveAcceptable(a)) << "the 2D engine would write a pitch the render hardware refuses";
}

// A reader that takes the texels as data (the CPU, another process, a blit, a kernel) makes the resolve exact through
// the complement, the same engine, at 2x and 4x; a screen-bound or pass-sampled target keeps the one-code route
TEST(ResolveRule, EveryWayTheTexelsCanBeReadAsDataTakesTheComplement)
{
    EXPECT_FALSE(raw::resolveTargetReadable({}));
    const raw::ResolveTarget each[] = {
        {true, false, false, false, false}, {false, true, false, false, false}, {false, false, true, false, false},
        {false, false, false, true, false}, {false, false, false, false, true},
    };
    for (const raw::ResolveTarget &t : each)
        for (uint8_t samples : {2, 4}) {
            EXPECT_TRUE(raw::resolveTargetReadable(t));
            raw::ResolveAsk a = screenBound(samples);
            a.readable = raw::resolveTargetReadable(t);
            EXPECT_FALSE(raw::twodResolveAcceptable(a));
            EXPECT_TRUE(raw::twodResolveExact(a));
            EXPECT_EQ(raw::resolveRoute(a), raw::ResolveByComplement);
            a.compressedSamples = true;
            EXPECT_EQ(raw::resolveRoute(a), raw::ResolveByPass) << "the complement on compressed samples is unproven";
        }
    for (uint8_t samples : {2, 4})
        EXPECT_EQ(raw::resolveRoute(screenBound(samples)),
                  raw::twodResolveComplementProven() && (samples == 2 || raw::twodResolveSingleRounding4x())
                      ? raw::ResolveByComplement
                      : raw::ResolveByTwod)
            << "a screen-bound target keeps the 2D engine's one code until the complement is proven";
    raw::ResolveAsk eight = screenBound(8);
    eight.readable = true;
    EXPECT_EQ(raw::resolveRoute(eight), raw::ResolveByPass);
}

TEST(ResolveRule, TheComplementWaitsOnTheDeviceAndThen4xOnOneRounding)
{
    const bool proven = raw::twodResolveComplementProven();
    EXPECT_EQ(raw::twodResolveExact(screenBound(2)), proven);
    EXPECT_EQ(raw::twodResolveExact(screenBound(4)), proven && raw::twodResolveSingleRounding4x());
    raw::ResolveAsk a = screenBound(2);
    a.compressedSamples = true;
    EXPECT_TRUE(raw::twodResolveAcceptable(a));
    EXPECT_FALSE(raw::twodResolveExact(a));
    a = screenBound(2);
    a.readable = true;
    EXPECT_TRUE(raw::twodResolveExact(a));
    EXPECT_EQ(raw::resolveSampleSlot(2), 0u);
    EXPECT_EQ(raw::resolveSampleSlot(4), 1u);
    EXPECT_EQ(raw::resolveSampleSlot(8), 2u);
}

TEST(ResolveArithmetic, OneRoundingIsTiesDownAndMissesMetalOnEveryTieAlone)
{
    for (uint32_t n : {2u, 4u})
        for (uint32_t s = 0; s <= 255 * n; s++) {
            EXPECT_EQ(direct(s, n), (2 * s + n - 1) / (2 * n)) << n << "x sum " << s;
            EXPECT_EQ(direct(s, n) != metal(s, n), s % n == n / 2) << n << "x sum " << s;
            EXPECT_EQ(metal(s, n) - direct(s, n), s % n == n / 2 ? 1u : 0u);
        }
}

TEST(ResolveArithmetic, TheComplementOfOneRoundingIsMetalsRuleForEverySum)
{
    for (uint32_t n : {2u, 4u})
        for (uint32_t s = 0; s <= 255 * n; s++)
            ASSERT_EQ(complemented(s, n), metal(s, n)) << n << "x sum " << s;
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t b = 0; b < 256; b++)
            ASSERT_EQ(complemented(a + b, 2), (a + b + 1) / 2) << a << "," << b;
}

TEST(ResolveArithmetic, TwoRoundedStepsAreWithinOneCodeAndNotExactEitherWay)
{
    uint32_t directMiss = 0, complementMiss = 0;
    for (uint32_t p = 0; p <= 510; p++)
        for (uint32_t q = 0; q <= 510; q++) {
            const uint32_t m = metal(p + q, 4);
            ASSERT_LE(std::abs(int(twoSteps(p, q)) - int(m)), 1);
            ASSERT_LE(std::abs(int(twoStepsComplemented(p, q)) - int(m)), 1);
            directMiss += twoSteps(p, q) != m;
            complementMiss += twoStepsComplemented(p, q) != m;
        }
    EXPECT_GT(directMiss, 0u);
    EXPECT_GT(complementMiss, 0u);
    EXPECT_EQ(twoStepsComplemented(1, 0), 1u);
    EXPECT_EQ(metal(1, 4), 0u);
}

TEST(ResolveCandidates, NoOriginBiasOfTheBilinearWeightsIsExact)
{
    for (uint32_t k = 0; k <= 256; k++) {
        bool missed = false;
        for (uint32_t a = 0; a < 256 && !missed; a++)
            for (uint32_t b = 0; b < 256 && !missed; b++)
                missed = store(uint64_t(256 - k) * a + uint64_t(k) * b, 256ull * 255) != metal(a + b, 2);
        EXPECT_TRUE(missed) << "weight " << k << "/256 on the second sample";
    }
    EXPECT_NE(store(128 * 0 + 128 * 1, 256ull * 255), metal(1, 2)) << "the unbiased box at samples (0, 1)";
    EXPECT_NE(store(127 * 1 + 129 * 0, 256ull * 255), metal(1, 2)) << "a bias toward b at samples (1, 0)";
    EXPECT_NE(store(129 * 0 + 127 * 1, 256ull * 255), metal(1, 2)) << "a bias toward a at samples (0, 1)";
}

TEST(ResolveCandidates, NoWiderIntermediateIsExact)
{
    const uint32_t a = 0, b = 1;
    const double mean = double(a + b) / 510.0;
    EXPECT_NE(storeDyadic(fp16TowardZero(mean)), metal(a + b, 2)) << "RGBA16F, then 1:1 to 8 bits";
    const uint32_t ten = store(a + b, 510, 10);
    EXPECT_NE(store(ten, 1023), metal(a + b, 2)) << "RGB10A2's colour, then 1:1 to 8 bits";
    EXPECT_NE((store(200, 255) >> 6) * 85, metal(200 + 200, 2)) << "RGB10A2's two-bit alpha";
    const uint32_t sixteen = store(a + b, 510, 16);
    EXPECT_NE(store(sixteen, 65535), metal(a + b, 2)) << "RGBA16Unorm, then 1:1 to 8 bits";
    for (uint32_t s = 0; s <= 510; s++)
        EXPECT_LE(storeDyadic(fp16TowardZero(double(s) / 510.0)), metal(s, 2)) << "toward zero only ever loses ties";
}

TEST(ResolveCandidates, NoTwoPassBlendOfTheEightBitBlenderIsExactAt2x)
{
    for (uint32_t fs = 0; fs < 256; fs++)
        for (uint32_t fd = 0; fd < 256; fd++) {
            bool missed = false;
            for (uint32_t x = 0; x < 256 && !missed; x++)
                for (uint32_t y = 0; y < 256 && !missed; y++)
                    missed = (y * fs + x * fd + 127) / 255 != metal(x + y, 2);
            ASSERT_TRUE(missed) << "Fs " << fs << " Fd " << fd;
        }
}

// Any 4x route whose first two samples meet in one 8-bit code fails: 511 pair sums share 256 codes, and every two of
// them are told apart by some sum of the other two samples
TEST(ResolveCandidates, NoEightBitIntermediateOfTwoSamplesIsExactAt4x)
{
    for (uint32_t p = 0; p <= 510; p++)
        for (uint32_t q = p + 1; q <= 510; q++) {
            bool apart = false;
            for (uint32_t t = 0; t <= 510 && !apart; t++)
                apart = metal(p + t, 4) != metal(q + t, 4);
            ASSERT_TRUE(apart) << p << " " << q;
        }
}

}  // namespace
