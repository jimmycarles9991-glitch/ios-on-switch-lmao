// Host test of drivers/NativeMobileDisplay/TegraDisplayColorMath.h against a model of DC A's CMU (LUT1, the 3x3 CSC
// with eight fraction bits, LUT2 indexed one bucket per value below 512 and one per eight above): the sRGB tables against
// the IEC 61966-2-1 curves, bit-exact passthrough with nothing asked for and with an identity CSC enabled, and what each
// IOMobileFramebuffer input does to the CSC words and the output curve.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../TegraDisplayColorMath.h"

using namespace switch_color;

namespace {

double decode(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double encode(double linear)
{
    return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
}

uint32_t bucketStart(unsigned k)
{
    return k < 512 ? k : 512 + (k - 512) * 8;
}

Q32 q32(double value)
{
    return Q32(std::llround(value * 4294967296.0));
}

int32_t cscSigned(uint16_t word)
{
    return word & 0x200 ? int32_t(word) - 0x400 : int32_t(word);
}

struct Rgb {
    uint32_t r, g, b;
};

// One pixel through the CMU as the tables program it; bypassed, it comes out as it went in.
Rgb scan(const Tables &tables, Rgb in)
{
    if (!tables.enabled)
        return in;
    const uint32_t channel[3] = {in.r, in.g, in.b};
    int64_t linear[3];
    for (int c = 0; c < 3; ++c)
        linear[c] = tables.lut1[channel[c]];
    uint32_t out[3];
    for (int r = 0; r < 3; ++r) {
        int64_t sum = 0;
        for (int c = 0; c < 3; ++c)
            sum += int64_t(cscSigned(tables.csc[r * 3 + c])) * linear[c];
        int64_t value = sum >> 8;
        value = value < 0 ? 0 : value > 4095 ? 4095 : value;
        out[r] = tables.lut2[lut2Index(uint32_t(value))];
    }
    return {out[0], out[1], out[2]};
}

Inputs defaults()
{
    Inputs in;
    identityGamma(in.gamma);
    return in;
}

Matrix diagonal(double r, double g, double b)
{
    Matrix m = {};
    m.m[0][0] = q32(r);
    m.m[1][1] = q32(g);
    m.m[2][2] = q32(b);
    return m;
}

}  // namespace

TEST(Tables, Lut1IsTheSrgbDecode)
{
    for (unsigned i = 0; i < kLut1Entries; ++i)
        EXPECT_EQ(kSrgbToLinear[i], std::lround(4095 * decode(i / 255.0))) << "LUT1 entry " << i;
}

TEST(Tables, Lut2IsTheSrgbEncodeExceptTheRoundTripBuckets)
{
    bool roundTrip[kLut2Entries] = {};
    for (unsigned i = 0; i < kLut1Entries; ++i)
        roundTrip[lut2Index(kSrgbToLinear[i])] = true;
    unsigned moved = 0;
    for (unsigned k = 0; k < kLut2Entries; ++k) {
        const long curve = std::lround(255 * encode(bucketStart(k) / 4095.0));
        if (kLinearToSrgb[k] == curve)
            continue;
        ++moved;
        EXPECT_TRUE(roundTrip[k]) << "LUT2 bucket " << k << " is off the curve";
        EXPECT_LE(std::labs(kLinearToSrgb[k] - curve), 1) << "LUT2 bucket " << k;
    }
    EXPECT_EQ(moved, 5u);
    for (unsigned k = 0; k + 1 < kLut2Entries; ++k)
        EXPECT_LE(kLinearToSrgb[k], kLinearToSrgb[k + 1]) << "LUT2 falls at bucket " << k;
}

TEST(Passthrough, NothingAskedForBypassesTheCmu)
{
    Tables tables;
    build(defaults(), tables);
    EXPECT_FALSE(tables.enabled);
    EXPECT_EQ(tables.reasons, 0u);
    for (unsigned i = 0; i < kCscWords; ++i)
        EXPECT_EQ(tables.csc[i], i % 4 == 0 ? kCscOne : 0) << "CSC word " << i;
}

TEST(Passthrough, IdentityCscEnabledIsBitExact)
{
    Tables tables;
    build(defaults(), tables);
    tables.enabled = true;
    for (uint32_t r = 0; r < 256; ++r)
        for (uint32_t g = 0; g < 256; g += 5)
            for (uint32_t b = 0; b < 256; b += 3) {
                const Rgb out = scan(tables, {r, g, b});
                ASSERT_TRUE(out.r == r && out.g == g && out.b == b) << r << "," << g << "," << b;
            }
}

TEST(Csc, WordFormatMatchesL4t)
{
    EXPECT_EQ(cscValue(kOne), 0x100);
    EXPECT_EQ(cscValue(q32(312 / 256.0)) & 0x3ff, 0x138);
    EXPECT_EQ(cscValue(q32(-70 / 256.0)) & 0x3ff, 0x3ba);
    EXPECT_EQ(cscValue(q32(5.0)), 511);
    EXPECT_EQ(cscValue(q32(-5.0)), -512);
}

TEST(Csc, NightShiftDiagonalScalesTheChannels)
{
    Inputs in = defaults();
    in.harmony = diagonal(1.0, 0.9, 0.8);
    Tables tables;
    build(in, tables);
    EXPECT_TRUE(tables.enabled);
    EXPECT_EQ(tables.reasons, uint32_t(kReasonMatrix));
    EXPECT_EQ(tables.csc[0], 256);
    EXPECT_EQ(tables.csc[4], 230);
    EXPECT_EQ(tables.csc[8], 205);
    const Rgb white = scan(tables, {255, 255, 255});
    EXPECT_EQ(white.r, 255u);
    EXPECT_LT(white.g, 255u);
    EXPECT_LT(white.b, white.g);
}

TEST(Csc, AccessibilityRunsBeforeTheWhitePoint)
{
    Inputs in = defaults();
    in.harmony = diagonal(0.5, 1.0, 1.0);
    in.accessibility.m[0][0] = 0;
    in.accessibility.m[0][1] = kOne;
    Tables tables;
    build(in, tables);
    EXPECT_EQ(tables.csc[0], 0);
    EXPECT_EQ(tables.csc[1], 128) << "red out is half of green in: harmony(accessibility(pixel))";
}

TEST(Remap, InvertedFlipsTheOutputAndNothingElse)
{
    Inputs in = defaults();
    in.remapMode = kRemapInverted;
    Tables tables;
    build(in, tables);
    EXPECT_TRUE(tables.enabled);
    EXPECT_EQ(tables.reasons, uint32_t(kReasonRemap));
    for (uint32_t v = 0; v < 256; ++v) {
        const Rgb out = scan(tables, {v, v, v});
        EXPECT_EQ(out.r, 255 - v) << v;
    }
}

TEST(Remap, GrayscaleKeepsWhiteAndRowsSumToOne)
{
    for (uint32_t mode : {kRemapGrayscale, kRemapGrayscaleInverted}) {
        Inputs in = defaults();
        in.remapMode = mode;
        Tables tables;
        build(in, tables);
        for (int r = 0; r < 3; ++r)
            EXPECT_EQ(cscSigned(tables.csc[r * 3]) + cscSigned(tables.csc[r * 3 + 1]) +
                          cscSigned(tables.csc[r * 3 + 2]),
                      256);
        const Rgb white = scan(tables, {255, 255, 255}), red = scan(tables, {255, 0, 0});
        EXPECT_EQ(white.r, mode == kRemapGrayscale ? 255u : 0u);
        EXPECT_TRUE(red.r == red.g && red.g == red.b) << "grey out of pure red";
    }
}

TEST(Remap, ModeTwoChangesNothing)
{
    Inputs in = defaults();
    in.remapMode = 2;
    Tables tables;
    build(in, tables);
    EXPECT_FALSE(tables.enabled);
}

TEST(Scale, ReduceWhitePointAndCorrectionMultiplyAndStopAtOne)
{
    Inputs in = defaults();
    in.accessibilityScale = kUnity16 / 2;
    Tables tables;
    build(in, tables);
    EXPECT_EQ(tables.csc[0], 128);
    EXPECT_EQ(tables.reasons, uint32_t(kReasonScale));
    in.correction = kUnity16 * 4;
    build(in, tables);
    EXPECT_FALSE(tables.enabled) << "0.5 x 4 stops at one: identity";
    in.correction = 0;
    in.accessibilityScale = 0;
    build(in, tables);
    EXPECT_FALSE(tables.enabled) << "an unset scale leaves the picture alone";
    in = defaults();
    in.harmony = Matrix{};
    in.accessibility = Matrix{};
    build(in, tables);
    EXPECT_FALSE(tables.enabled) << "an unset matrix leaves the picture alone";
}

TEST(Contrast, FloatArgumentAndNeutralValues)
{
    uint32_t value = 0;
    ASSERT_TRUE(floatToUnity16(0x3f800000, &value));
    EXPECT_EQ(value, kUnity16);
    ASSERT_TRUE(floatToUnity16(0x3f000000, &value));
    EXPECT_EQ(value, kUnity16 / 2);
    ASSERT_TRUE(floatToUnity16(0x40000000, &value));
    EXPECT_EQ(value, kUnity16 * 2);
    ASSERT_TRUE(floatToUnity16(0, &value));
    EXPECT_EQ(value, 0u);
    EXPECT_FALSE(floatToUnity16(0xbf800000, &value));
    EXPECT_FALSE(floatToUnity16(0x7fc00000, &value));
    Inputs in = defaults();
    in.contrast = 0;
    Tables tables;
    build(in, tables);
    EXPECT_FALSE(tables.enabled);
    in.contrast = kUnity16 * 2;
    build(in, tables);
    EXPECT_TRUE(tables.enabled);
    EXPECT_EQ(scan(tables, {255, 255, 255}).r, 255u);
    EXPECT_EQ(scan(tables, {0, 0, 0}).r, 0u);
    EXPECT_EQ(scan(tables, {64, 64, 64}).r, 1u);
}

TEST(Gamma, IdentityTableIsIdentityAndATableApplies)
{
    Inputs in = defaults();
    for (unsigned point = 0; point < 256; ++point)
        EXPECT_EQ((gammaIdentity(point) + 2) >> 2, point);
    in.gamma[128] = 100;
    Tables tables;
    build(in, tables);
    EXPECT_TRUE(tables.enabled);
    EXPECT_EQ(tables.reasons, uint32_t(kReasonGamma));
    EXPECT_EQ(scan(tables, {128, 128, 128}).r, 100u);
    EXPECT_EQ(scan(tables, {127, 127, 127}).r, 127u);
}

TEST(Blank, BlackWhateverTheWindowsAndTheFilters)
{
    for (uint32_t mode : {kRemapNormal, kRemapInverted, kRemapGrayscaleInverted}) {
        Inputs in = defaults();
        in.remapMode = mode;
        in.blank = true;
        Tables tables;
        build(in, tables);
        EXPECT_TRUE(tables.enabled);
        EXPECT_TRUE(tables.reasons & kReasonBlank);
        for (uint32_t v = 0; v < 256; v += 15) {
            const Rgb out = scan(tables, {v, 255 - v, v / 2});
            EXPECT_TRUE(out.r == 0 && out.g == 0 && out.b == 0) << "mode " << mode << " value " << v;
        }
    }
}
