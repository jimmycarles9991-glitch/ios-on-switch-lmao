// SPDX-License-Identifier: MIT
// Rule resolve.twod (raw/runtime/raw_resolve_rule.h): where it is taken, and the arithmetic it rests on, exhaustively,
// against the ROP's unorm store rule (x1-units UNITS.md): u = floor(v * 2^(n+4)), code = (u * (2^n - 1) + 2^(n+3) - 1)
// >> (n+4); RGBA16F toward zero; the 8-bit blender round((S*Fs + D*Fd) / 255); and the 2D engine's reading of a unorm8
// code at 15 bits. Each candidate for an exact 2D-engine resolve that fails is held to the counter-example it fails on.
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
    a.format = 91;
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

// A reader that takes the texels as data (the CPU, another process, a blit, a kernel) gets the pack's resolve pass, at
// 2x and 4x: Metal's rule on the chip and faster than the complement; a screen-bound or pass-sampled target keeps the
// one-code route
TEST(ResolveRule, EveryWayTheTexelsCanBeReadAsDataTakesThePass)
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
            EXPECT_EQ(raw::resolveRoute(a), raw::ResolveByPass);
        }
    for (uint8_t samples : {2, 4})
        EXPECT_EQ(raw::resolveRoute(screenBound(samples)), raw::ResolveByTwod);
    raw::ResolveAsk eight = screenBound(8);
    eight.readable = true;
    EXPECT_EQ(raw::resolveRoute(eight), raw::ResolveByPass);
}

raw::ResolveAsk ask(uint16_t format, uint8_t samples, bool readable)
{
    raw::ResolveAsk a = screenBound(samples);
    a.format = format;
    a.readable = readable;
    return a;
}

// gpu.raw.resolve_twod rgba16f_2x (exact, rgba16f_2x_read=twod) and rgba16f_4x (near)
TEST(ResolveRule, RGBA16FloatTakesTheEngineEverywhereAt2xAndScreenBoundAt4x)
{
    EXPECT_EQ(raw::resolveRoute(ask(115, 2, false)), raw::ResolveByTwod);
    EXPECT_EQ(raw::resolveRoute(ask(115, 2, true)), raw::ResolveByTwod);
    EXPECT_EQ(raw::resolveRoute(ask(115, 4, false)), raw::ResolveByTwod);
    EXPECT_EQ(raw::resolveRoute(ask(115, 4, true)), raw::ResolveByPass);
    EXPECT_FALSE(raw::twodResolveComplementShape(ask(115, 2, false)));
    EXPECT_TRUE(raw::twodResolveComplementShape(ask(80, 2, true)));
}

// gpu.raw.resolve_twod bgra8srgb_<n>x and rgba8srgb_<n>x (near)
TEST(ResolveRule, SRGBResolvesTakeTheEngineScreenBound)
{
    for (uint16_t format : {71, 81})
        for (uint8_t samples : {2, 4}) {
            EXPECT_EQ(raw::resolveRoute(ask(format, samples, false)), raw::ResolveByTwod);
            EXPECT_EQ(raw::resolveRoute(ask(format, samples, true)), raw::ResolveByPass);
            EXPECT_FALSE(raw::twodResolveComplementShape(ask(format, samples, false)));
        }
}

// gpu.raw.resolve_twod rgb10a2_<n>x (near)
TEST(ResolveRule, ScreenBoundRGB10A2TakesTheEngine)
{
    for (uint8_t samples : {2, 4}) {
        EXPECT_EQ(raw::resolveRoute(ask(90, samples, false)), raw::ResolveByTwod);
        EXPECT_EQ(raw::resolveRoute(ask(90, samples, true)), raw::ResolveByPass);
    }
}

// gpu.raw.resolve_twod rg11b10f_<n>x (near)
TEST(ResolveRule, ScreenBoundRG11B10FloatTakesTheEngine)
{
    for (uint8_t samples : {2, 4}) {
        EXPECT_EQ(raw::resolveRoute(ask(92, samples, false)), raw::ResolveByTwod);
        EXPECT_EQ(raw::resolveRoute(ask(92, samples, true)), raw::ResolveByPass);
    }
}

TEST(ResolveRule, TheComplementIsNoRoute)
{
    for (uint16_t format : {70, 80})
        for (uint8_t samples : {2, 4, 8})
            for (int bits = 0; bits < 8; bits++) {
                raw::ResolveAsk a;
                a.format = format;
                a.samples = samples;
                a.readable = bits & 1;
                a.wholeTarget = bits & 2;
                a.shadowTarget = bits & 4;
                EXPECT_NE(raw::resolveRoute(a), raw::ResolveByComplement);
            }
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

// The 2D engine's 2x box as the chip reads it: each code at 15 bits, nearest, then the store rule of the exact mean
uint32_t code15(uint32_t c)
{
    return (257 * c + 1) >> 1;
}

uint32_t engineBox(uint32_t a, uint32_t b)
{
    const uint32_t u = (code15(a) + code15(b)) >> 4;
    return (u * 255 + 2047) >> 12;
}

uint32_t engineComplement(uint32_t a, uint32_t b)
{
    return 255 - engineBox(255 - a, 255 - b);
}

// hw_test_app/shaders/ResolveTwod.metal's sample of pixel (x, y)
uint32_t sampleHash(uint32_t x, uint32_t y, uint32_t sample)
{
    uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ ((sample + 1) * 83492791u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

TEST(ResolveEngine, CodesAtFifteenBitsGiveTheDevicesCounts)
{
    for (uint32_t c = 0; c < 256; c++)
        ASSERT_EQ(code15(c), uint32_t(std::lround(c * 32768.0 / 255.0))) << c;
    // gpu.raw.resolve_twod's 720x1280 BGRA8 pass at 2x, boots 19 and 21: 2x_one_off_bgra and 2x_exact_one_off_bgra
    const uint32_t shift[4] = {16, 8, 0, 24};
    const uint32_t box[4] = {447364, 448690, 447877, 452349}, complement[4] = {13130, 12545, 13530, 12716};
    uint32_t boxOff[4] = {}, complementOff[4] = {};
    for (uint32_t y = 0; y < 1280; y++)
        for (uint32_t x = 0; x < 720; x++) {
            const uint32_t h0 = sampleHash(x, y, 0), h1 = sampleHash(x, y, 1);
            for (int c = 0; c < 4; c++) {
                const uint32_t a = h0 >> shift[c] & 255, b = h1 >> shift[c] & 255, m = metal(a + b, 2);
                boxOff[c] += engineBox(a, b) != m;
                complementOff[c] += engineComplement(a, b) != m;
            }
        }
    for (int c = 0; c < 4; c++) {
        EXPECT_EQ(boxOff[c], box[c]) << "channel " << c;
        EXPECT_EQ(complementOff[c], complement[c]) << "channel " << c;
    }
}

// Where the box and its complement both land one under Metal, the pair (k, k) gives the same two codes and Metal's k,
// so no function of the two readings, a ROP3 among them, is Metal's rule
TEST(ResolveEngine, NoReadingOfTheBoxAndItsComplementIsExact)
{
    uint32_t both = 0, complementOff = 0;
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t b = 0; b < 256; b++) {
            const uint32_t m = metal(a + b, 2), box = engineBox(a, b), complement = engineComplement(a, b);
            ASSERT_LE(m - box, 1u);
            ASSERT_LE(m - complement, 1u);
            complementOff += complement != m;
            if (box == m || complement == m)
                continue;
            both++;
            const uint32_t k = box;
            ASSERT_EQ(complement, k);
            ASSERT_EQ(engineBox(k, k), k);
            ASSERT_EQ(engineComplement(k, k), k);
            ASSERT_EQ(metal(2 * k, 2), k);
        }
    EXPECT_EQ(both, 896u);
    EXPECT_EQ(complementOff, 896u) << "every complement miss is a box miss too";
}

}  // namespace
