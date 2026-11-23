// SPDX-License-Identifier: MIT
// zbc.h's entries past the 8-bit unorm and half ones, from the clear words clear.h hands the 3D
// engine: each holds the texel the ROP writes for the clear, packed in the format's channel order, or none is made.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../clear.h"
#include "../zbc.h"

namespace {

bool entry(uint32_t mtl, const float rgba[4], TegraGM20BAbiZbc &e)
{
    uint32_t ds[4];
    if (!gm20b::clearValue(mtl, rgba, ds))
        return false;
    return gm20b::zbcColorEntry(mtl, ds, e);
}

float linear(uint32_t code)
{
    const double e = code / 255.0;
    return float(e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4));
}

// gpu.gm20b.compressed_targets zbc_bgra8srgb and zbc_rgba8srgb
TEST(GM20BZbc, SrgbEntriesHoldTheEncodedCodes)
{
    TegraGM20BAbiZbc e;
    for (uint32_t k = 0; k < 256; k++) {
        const float rgba[4] = {linear(k), linear(255 - k), linear(k / 2), k / 255.0f};
        ASSERT_TRUE(entry(81, rgba, e)) << k;
        EXPECT_EQ(e.format, gm20b::kZbcA8Rl8Gl8Bl8);
        EXPECT_EQ(e.l2[0], (k / 2) | (255 - k) << 8 | k << 16 | k << 24) << k;
        ASSERT_TRUE(entry(71, rgba, e)) << k;
        EXPECT_EQ(e.format, gm20b::kZbcA8Bl8Gl8Rl8);
        EXPECT_EQ(e.l2[0], k | (255 - k) << 8 | (k / 2) << 16 | k << 24) << k;
    }
    const float off[4] = {0.5f, 0.5f, 0.5f, 1};
    ASSERT_TRUE(entry(81, off, e)) << "clear.h hands a code's linear value for any colour";
    EXPECT_EQ(e.l2[0], 0xffbcbcbcu);
    uint32_t ds[4] = {gm20b::clear_detail::bitsOf(0.5f), 0, 0, gm20b::clear_detail::bitsOf(1.0f)};
    EXPECT_FALSE(gm20b::zbcColorEntry(81, ds, e)) << "a linear value between two codes";
}

// gpu.gm20b.compressed_targets zbc_rgb10a2
TEST(GM20BZbc, Rgb10a2EntriesPackRedLowAlphaHigh)
{
    TegraGM20BAbiZbc e;
    const float codes[4] = {1.0f, 0.0f, 512 / 1023.0f, 2 / 3.0f};
    ASSERT_TRUE(entry(90, codes, e));
    EXPECT_EQ(e.format, gm20b::kZbcA2B10G10R10);
    EXPECT_EQ(e.l2[0], 1023u | 512u << 20 | 2u << 30);
    const float any[4] = {0.3f, 0.6f, 0.9f, 0.5f};
    ASSERT_TRUE(entry(90, any, e)) << "clear.h hands exact codes";
    EXPECT_EQ(e.l2[0], 307u | 614u << 10 | 921u << 20 | 2u << 30);
    uint32_t ds[4] = {gm20b::clear_detail::bitsOf(0.3f), 0, 0, gm20b::clear_detail::bitsOf(1.0f)};
    EXPECT_FALSE(gm20b::zbcColorEntry(90, ds, e)) << "306.9: no code";
}

// gpu.gm20b.compressed_targets zbc_rgba16unorm
TEST(GM20BZbc, Rgba16UnormEntriesPackTwoChannelsAWord)
{
    TegraGM20BAbiZbc e;
    const float codes[4] = {10000 / 65535.0f, 20000 / 65535.0f, 40000 / 65535.0f, 1.0f};
    ASSERT_TRUE(entry(110, codes, e));
    EXPECT_EQ(e.format, gm20b::kZbcR16x4);
    EXPECT_EQ(e.l2[0], 10000u | 20000u << 16);
    EXPECT_EQ(e.l2[1], 40000u | 65535u << 16);
    uint32_t ds[4] = {gm20b::clear_detail::bitsOf(0.5f), 0, 0, gm20b::clear_detail::bitsOf(1.0f)};
    EXPECT_FALSE(gm20b::zbcColorEntry(110, ds, e)) << "32767.5: the ROP's tie";
}

// gpu.gm20b.compressed_targets zbc_rgba32f
TEST(GM20BZbc, Rgba32FloatEntriesHoldTheWords)
{
    TegraGM20BAbiZbc e;
    const float values[4] = {0.3f, -1.5f, 7.25f, 0.5f};
    ASSERT_TRUE(entry(125, values, e));
    EXPECT_EQ(e.format, gm20b::kZbcRf32x4);
    EXPECT_EQ(memcmp(e.l2, e.ds, sizeof e.l2), 0);
    EXPECT_EQ(e.l2[1], gm20b::clear_detail::bitsOf(-1.5f));
    const float nan[4] = {NAN, 0, 0, 1};
    EXPECT_FALSE(entry(125, nan, e));
}

// gpu.gm20b.compressed_targets zbc_depth16
TEST(GM20BZbc, Depth16ClearsTakeTheFloatDepthEntry)
{
    TegraGM20BAbiZbc d16, d32;
    const float depth = 39321 / 65535.0f;
    ASSERT_TRUE(gm20b::zbcDepthEntry(250, depth, d16));
    ASSERT_TRUE(gm20b::zbcDepthEntry(252, depth, d32));
    EXPECT_EQ(memcmp(&d16, &d32, sizeof d16), 0) << "one entry for both: the GPU matches a depth by its float";
    EXPECT_EQ(d16.format, uint32_t(gm20b::kZbcDepthFp32));
    EXPECT_FALSE(gm20b::zbcDepthEntry(253, depth, d16)) << "Stencil8";
}

}  // namespace
