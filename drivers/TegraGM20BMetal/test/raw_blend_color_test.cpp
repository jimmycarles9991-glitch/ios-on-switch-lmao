// SPDX-License-Identifier: MIT
// raw_blend_color.h: the cases raw_blend_words_test.rs holds m2v's conversion to, over raw's. The two agree on every f32
// in every lane (an exhaustive comparison when the conversion moved into raw).
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_blend_color.h"

namespace {

struct Words {
    uint32_t w[8];
};

Words words(float r, float g, float b, float a)
{
    const float color[4] = {r, g, b, a};
    Words out;
    raw::blendColorWords(color, out.w);
    return out;
}

float value(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

uint32_t bits(float f)
{
    return raw::blend_color::bits(f);
}

TEST(RawBlendColor, ClampsNanInfinityAndAlpha)
{
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const Words w = words(nan, inf, -inf, nan);
    const float want[8] = {-1542.0f / 2048,       1710.0f / 1024,        -1542.0f / 2048,       0.0f,
                           1.0f + 1542.0f / 2048, 1.0f - 1281.0f / 1024, 1.0f + 1542.0f / 2048, 1.0f};
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(value(w.w[i]), want[i]) << i;
    EXPECT_EQ(words(0, 0, 0, inf).w[3], bits(1.0f));
    EXPECT_EQ(words(0, 0, 0, -inf).w[3], 0u);
}

TEST(RawBlendColor, RoundsBothColourAndOneMinusAsMetalReadsThem)
{
    const Words w = words(0.3f, 1.2f, -0.4f, 0.45f);
    const uint32_t want[8] = {0x3e99a000, 0x3f99a000, 0xbeccc000, 0x3ee66000,
                              0x3f334000, 0xbe4cc000, 0x3fb34000, 0x3f0cc000};
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(w.w[i], want[i]) << i;
}

TEST(RawBlendColor, DistinguishesTheSrgbDirectBoundary)
{
    const uint32_t boundary = 0x3fd59fd3;
    const Words w = words(value(boundary - 1), value(boundary), value(boundary + 1), 1.0f);
    EXPECT_EQ(value(w.w[0]), 1709.0f / 1024);
    EXPECT_EQ(value(w.w[1]), 1710.0f / 1024);
    EXPECT_EQ(value(w.w[2]), 1710.0f / 1024);
    for (int i = 4; i < 7; i++)
        EXPECT_EQ(value(w.w[i]), 1.0f - 1281.0f / 1024) << i;
}

TEST(RawBlendColor, HalfTiesAreEvenAndColourZeroIsPositive)
{
    for (const float sign : {1.0f, -1.0f}) {
        const Words w = words(sign * (0.5f + 1.0f / 4096), sign * (0.5f + 3.0f / 4096), -0.0f, -0.0f);
        EXPECT_EQ(w.w[0], bits(sign * 0.5f));
        EXPECT_EQ(w.w[1], bits(sign * (0.5f + 1.0f / 1024)));
        EXPECT_EQ(w.w[2], 0u);
        EXPECT_EQ(w.w[3], 0u);
    }
}

TEST(RawBlendColor, RoundsHalfSubnormalMidpoints)
{
    const float step = std::ldexp(1.0f, -24);
    const Words w = words(step / 2, step * 1.5f, step * 2.5f, 0.0f);
    EXPECT_EQ(w.w[0], 0u);
    EXPECT_EQ(w.w[1], bits(step * 2));
    EXPECT_EQ(w.w[2], bits(step * 2));
}

}  // namespace
