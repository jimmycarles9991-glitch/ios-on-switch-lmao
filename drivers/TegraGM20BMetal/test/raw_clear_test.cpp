// SPDX-License-Identifier: MIT
// raw/runtime/raw_clear.h: the clear values the raw path hands NVK, m2v's (metal2vulkan-runtime clear_value.rs `color`) for
// every format it renders.
#include "../raw/runtime/raw_clear.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

namespace {

float asFloat(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void clear(uint32_t format, float r, float g, float b, float a, float out[4])
{
    const float in[4] = {r, g, b, a};
    uint32_t v[4];
    ASSERT_TRUE(raw::clearValue(format, in, v));
    for (int i = 0; i < 4; i++)
        out[i] = asFloat(v[i]);
}

TEST(RawClear, NormalizedChannelsAreTheNearestCode)
{
    float v[4];
    clear(80, 0.5f, 1.5f, -1.0f, 0.2f, v);
    EXPECT_EQ(v[0], float(128.0 / 255.0));
    EXPECT_EQ(v[1], 1.0f);
    EXPECT_EQ(v[2], 0.0f);
    EXPECT_EQ(v[3], float(51.0 / 255.0));
    clear(60, 0.5f, 0.25f, 9.0f, 9.0f, v);
    EXPECT_EQ(v[0], float(32768.0 / 65535.0));
    EXPECT_EQ(v[1], float(16384.0 / 65535.0));
    EXPECT_EQ(v[2], 0.0f);
    EXPECT_EQ(v[3], 0.0f);
    clear(94, 0.5f, 0.5f, 0.5f, 0.5f, v);
    EXPECT_EQ(v[0], float(512.0 / 1023.0));
    EXPECT_EQ(v[3], float(2.0 / 3.0));
}

TEST(RawClear, AnAlphaFormatClearsAlphaAlone)
{
    float v[4];
    clear(1, 1.0f, 1.0f, 1.0f, 0.5f, v);
    EXPECT_EQ(v[0], 0.0f);
    EXPECT_EQ(v[2], 0.0f);
    EXPECT_EQ(v[3], float(128.0 / 255.0));
}

TEST(RawClear, HalfChannelsAreTheNearestHalf)
{
    float v[4];
    clear(65, 0.1f, 70000.0f, 0.3f, 0.4f, v);
    EXPECT_EQ(v[0], float(_Float16(0.1f)));
    EXPECT_TRUE(std::isinf(v[1]));
    EXPECT_EQ(v[2], 0.0f);
    EXPECT_EQ(v[3], 0.0f);
}

TEST(RawClear, PackedSmallChannelsTakeTheHalfFirst)
{
    float v[4];
    clear(40, 0.5f, 0.5f, 0.5f, 1.0f, v);
    EXPECT_EQ(v[0], float(16.0 / 31.0));
    EXPECT_EQ(v[1], float(32.0 / 63.0));
    EXPECT_EQ(v[3], 0.0f);
    clear(43, 0.0f, 0.0f, 0.0f, 0.6f, v);
    EXPECT_EQ(v[3], 1.0f);
}

TEST(RawClear, SrgbChannelsDecodeToTheirCode)
{
    float v[4];
    clear(81, 0.5f, 0.0f, 1.0f, 0.5f, v);
    const double code = std::nearbyint((1.055 * std::pow(0.5, 1.0 / 2.4) - 0.055) * 255.0) / 255.0;
    EXPECT_EQ(v[0], float(std::pow((code + 0.055) / 1.055, 2.4)));
    EXPECT_EQ(v[1], 0.0f);
    EXPECT_EQ(v[2], 1.0f);
    EXPECT_EQ(v[3], float(128.0 / 255.0));
}

TEST(RawClear, SmallFloatsTruncate)
{
    float v[4];
    clear(92, 1.0f + 1.0f / 32.0f, 1.0f + 1.0f / 128.0f, -2.0f, 3.0f, v);
    EXPECT_EQ(v[0], 1.0f + 1.0f / 32.0f);
    EXPECT_EQ(v[1], 1.0f);
    EXPECT_EQ(v[2], 0.0f);
    EXPECT_EQ(v[3], 0.0f);
    clear(92, 1e9f, 0.0f, 0.0f, 0.0f, v);
    EXPECT_EQ(v[0], float((2.0 - 1.0 / 64.0) * 32768.0));
}

TEST(RawClear, FloatNansAreQuieted)
{
    uint32_t snan = 0x7f800001u;
    float in[4];
    memcpy(&in[0], &snan, 4);
    in[1] = in[2] = in[3] = 1.0f;
    uint32_t v[4];
    ASSERT_TRUE(raw::clearValue(105, in, v));
    EXPECT_EQ(v[0], 0x7fc00001u);
    EXPECT_EQ(v[2], 0u);
}

TEST(RawClear, IntegerChannelsTruncateAndSaturate)
{
    const double in[4] = {300.9, -5.0, std::nan(""), 7.99};
    float slots[4];
    ASSERT_TRUE(raw::integerClearSlots(73, in, slots));  // RGBA8Uint
    uint32_t v[4];
    ASSERT_TRUE(raw::clearValue(73, slots, v));
    EXPECT_EQ(v[0], 255u);
    EXPECT_EQ(v[1], 0u);
    EXPECT_EQ(v[2], 0u);
    EXPECT_EQ(v[3], 7u);
    const double wide[4] = {-200.5, 1e12, -1e12, 3.0};
    ASSERT_TRUE(raw::integerClearSlots(114, wide, slots));  // RGBA16Sint
    ASSERT_TRUE(raw::clearValue(114, slots, v));
    EXPECT_EQ(v[0], uint32_t(-200));
    EXPECT_EQ(v[1], 32767u);
    EXPECT_EQ(v[2], uint32_t(-32768));
    EXPECT_EQ(v[3], 3u);
    const double big[4] = {16777217.0, 4294967296.0, 0, 0};
    ASSERT_TRUE(raw::integerClearSlots(103, big, slots));  // RG32Uint: past a float's exact range
    ASSERT_TRUE(raw::clearValue(103, slots, v));
    EXPECT_EQ(v[0], 16777217u);
    EXPECT_EQ(v[1], 0xffffffffu);
    EXPECT_EQ(v[2], 0u);
    const double ten[4] = {1500, 2.0, 3.0, 9.0};
    ASSERT_TRUE(raw::integerClearSlots(91, ten, slots));  // RGB10A2Uint
    ASSERT_TRUE(raw::clearValue(91, slots, v));
    EXPECT_EQ(v[0], 1023u);
    EXPECT_EQ(v[3], 3u);
    EXPECT_FALSE(raw::integerClearSlots(80, ten, slots));
}

TEST(RawClear, AFormatNotRenderedIsRefused)
{
    const float in[4] = {};
    uint32_t v[4];
    EXPECT_FALSE(raw::clearValue(93, in, v));
}

}  // namespace
