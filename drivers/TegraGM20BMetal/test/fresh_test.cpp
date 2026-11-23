// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "../fresh.h"

namespace {

constexpr uint64_t kShaderRead = 0x1, kRenderTarget = 0x4;
constexpr uint64_t kTarget = kShaderRead | kRenderTarget;

std::vector<uint8_t> texel(uint32_t format)
{
    gm20b::FreshTexel t;
    if (!gm20b::freshPatternTexel(format, t))
        return {};
    return std::vector<uint8_t>(t.bytes, t.bytes + t.size);
}

// What the copy engine stores in `texels` texels under `fill`: each destination component its constant's low bytes
std::vector<uint8_t> copyEngineWrites(const gm20b::CopyFill &fill, uint32_t texels)
{
    std::vector<uint8_t> out;
    for (uint32_t t = 0; t < texels; t++)
        for (uint32_t c = 0; c < fill.components; c++) {
            const uint32_t code = fill.remap >> (4 * c) & 7;
            const uint32_t value = code == gm20b::kRemapConstA   ? fill.constA
                                   : code == gm20b::kRemapConstB ? fill.constB
                                                                 : 0xdeadbeef;
            for (uint32_t b = 0; b < fill.componentBytes; b++)
                out.push_back(uint8_t(value >> (8 * b)));
        }
    return out;
}

}  // namespace

// The texels an M4 Max's blits read from new Private textures (out/agent-scratch/x1-fresh/mac/fresh.mm, 11 Nov)
TEST(GM20BFresh, PatternTexelsAreTheOnesAppleReads)
{
    EXPECT_EQ(texel(80), (std::vector<uint8_t>{0xff, 0x00, 0xff, 0xff}));
    EXPECT_EQ(texel(70), (std::vector<uint8_t>{0xff, 0x00, 0xff, 0xff}));
    EXPECT_EQ(texel(115), (std::vector<uint8_t>{0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff}));
    EXPECT_EQ(texel(55), (std::vector<uint8_t>{0xff, 0xff, 0xff, 0xff}));
    EXPECT_EQ(texel(10), (std::vector<uint8_t>{0xff}));
    EXPECT_EQ(texel(30), (std::vector<uint8_t>{0xff, 0x00}));
    EXPECT_EQ(texel(90), (std::vector<uint8_t>{0xff, 0x03, 0xf0, 0xff}));
    EXPECT_EQ(texel(125), (std::vector<uint8_t>{0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                0xff, 0xff}));
    EXPECT_TRUE(texel(130).empty());
    EXPECT_TRUE(texel(550).empty());
    EXPECT_TRUE(texel(252).empty());
}

TEST(GM20BFresh, OnlyTexturesMetalCompressesReadThePattern)
{
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 64, 64, 1), 1u);
    EXPECT_EQ(gm20b::patternLevels(kTarget | gm20b::kUsageShaderWrite, true, 64, 64, 1), 0u);
    EXPECT_EQ(gm20b::patternLevels(kTarget | gm20b::kUsagePixelFormatView, true, 64, 64, 1), 0u);
    EXPECT_EQ(gm20b::patternLevels(kTarget | gm20b::kUsageShaderAtomic, true, 64, 64, 1), 0u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, false, 64, 64, 1), 0u);
    EXPECT_EQ(gm20b::patternLevels(0, true, 64, 64, 1), 0u);
    EXPECT_EQ(gm20b::patternLevels(kShaderRead, true, 64, 64, 1), 1u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 15, 64, 1), 0u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 1280, 108, 1), 1u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 832, 1408, 1), 1u);
}

// A chain's levels down to a shorter side of 16 (the M4 Max: 64, 32 and 16 of a 64 x 64 chain; m2v's pattern_levels table)
TEST(GM20BFresh, ChainLevelsBelowSixteenReadZero)
{
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 64, 64, 7), 3u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 100, 40, 7), 2u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 1024, 32, 11), 2u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 128, 16, 8), 1u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 16, 256, 9), 1u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 17, 17, 5), 1u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 15, 15, 4), 0u);
    EXPECT_EQ(gm20b::patternLevels(kTarget, true, 4096, 1, 1), 0u);
}

TEST(GM20BFresh, TheCopyEngineWritesEveryPatternExactly)
{
    for (uint32_t format = 0; format < 600; format++) {
        gm20b::FreshTexel t;
        if (!gm20b::freshPatternTexel(format, t))
            continue;
        gm20b::CopyFill fill;
        ASSERT_TRUE(gm20b::copyFillOf(t.bytes, t.size, fill)) << format;
        EXPECT_EQ(uint32_t(fill.componentBytes) * fill.components, t.size) << format;
        std::vector<uint8_t> want;
        for (int n = 0; n < 3; n++)
            want.insert(want.end(), t.bytes, t.bytes + t.size);
        EXPECT_EQ(copyEngineWrites(fill, 3), want) << format;
    }
    const uint8_t zeros[16] = {};
    for (uint8_t size : {1, 2, 4, 8, 16}) {
        gm20b::CopyFill fill;
        ASSERT_TRUE(gm20b::copyFillOf(zeros, size, fill));
        EXPECT_EQ(copyEngineWrites(fill, 2), std::vector<uint8_t>(2u * size, 0));
    }
}

TEST(GM20BFresh, RemapWordIsTheClassLayout)
{
    gm20b::FreshTexel t;
    ASSERT_TRUE(gm20b::freshPatternTexel(115, t));
    gm20b::CopyFill fill;
    ASSERT_TRUE(gm20b::copyFillOf(t.bytes, t.size, fill));
    EXPECT_EQ(fill.constA, 0x0000ffffu);
    EXPECT_EQ(fill.constB, 0xffffffffu);
    EXPECT_EQ(fill.remap, 4u | 5u << 4 | 4u << 8 | 4u << 12 | 3u << 16 | 1u << 20 | 1u << 24);
    ASSERT_TRUE(gm20b::freshPatternTexel(80, t));
    ASSERT_TRUE(gm20b::copyFillOf(t.bytes, t.size, fill));
    EXPECT_EQ(fill.constA, 0xffff00ffu);
    EXPECT_EQ(fill.remap, 0x4444u | 3u << 16);
}

TEST(GM20BFresh, ATexelOfThreeWordsHasNoFill)
{
    const uint8_t three[16] = {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0};
    gm20b::CopyFill fill;
    EXPECT_FALSE(gm20b::copyFillOf(three, 16, fill));
    EXPECT_FALSE(gm20b::copyFillOf(three, 12, fill));
}

// A pass that sets no renderTargetWidth or renderTargetHeight renders over its smallest attachment whole: a new target it
// clears and stores needs no fill first
TEST(GM20BFresh, ARenderAreaNotSetIsTheSmallestAttachmentsWhole)
{
    EXPECT_EQ(gm20b::renderAreaSide(0, 640), 640u);
    EXPECT_EQ(gm20b::renderAreaSide(50, 640), 50u);
    EXPECT_EQ(gm20b::renderAreaSide(800, 640), 640u);
    EXPECT_TRUE(gm20b::firstPassWritesAll(1280, 720, 1, 1, true, true, false, gm20b::renderAreaSide(0, 1280),
                                          gm20b::renderAreaSide(0, 720)));
    EXPECT_FALSE(gm20b::firstPassWritesAll(1280, 720, 1, 1, true, true, false, gm20b::renderAreaSide(0, 1280),
                                           gm20b::renderAreaSide(0, 360)));
}

// Core Animation's backdrop capture: a 640 x 64 texture cleared over a 640 x 50 render area (the hard edge effect's trace,
// x1-edgeline/cat/hard6.txt), which Metal's clear reaches to 64 rows
TEST(GM20BFresh, AFirstClearReachingWholeTilesWritesEverything)
{
    EXPECT_TRUE(gm20b::firstPassWritesAll(640, 64, 1, 1, true, true, false, 640, 50));
    EXPECT_TRUE(gm20b::firstPassWritesAll(1280, 108, 1, 1, true, true, false, 1280, 107));
    EXPECT_FALSE(gm20b::firstPassWritesAll(1280, 108, 1, 1, true, true, false, 1280, 60));
    EXPECT_FALSE(gm20b::firstPassWritesAll(640, 64, 1, 1, false, true, false, 640, 64));
    EXPECT_FALSE(gm20b::firstPassWritesAll(640, 64, 1, 1, true, false, false, 640, 64));
    EXPECT_FALSE(gm20b::firstPassWritesAll(640, 64, 1, 1, true, true, true, 640, 64));
    EXPECT_FALSE(gm20b::firstPassWritesAll(64, 64, 7, 1, true, true, false, 64, 64));
    EXPECT_FALSE(gm20b::firstPassWritesAll(64, 64, 1, 6, true, true, false, 64, 64));
}
