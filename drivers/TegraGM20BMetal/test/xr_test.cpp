// SPDX-License-Identifier: MIT
// xr.h: the extended-range formats the driver keeps, and the codes Metal's Clear stores in them. The texels
// below are an Apple M4 Max's, read back by harness/xroracle.mm, which holds xrClearTexel to Metal's clears on this Mac's GPU
// over 755 colors of each linear form by both routes, and pack/xr.metal's stand-in to Metal's reads of every code.
#include "../xr.h"
#include "../xr_words.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct Texel {
    uint32_t w[4];
};

Texel clear(gm20b::XrForm form, float r, float g, float b, float a, bool straight = false)
{
    const float color[4] = {r, g, b, a};
    Texel t{};
    gm20b::xrClearTexel(form, color, straight, t.w);
    return t;
}

void expectTexel(const Texel &t, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    EXPECT_EQ(t.w[0], w0);
    EXPECT_EQ(t.w[1], w1);
    EXPECT_EQ(t.w[2], w2);
    EXPECT_EQ(t.w[3], w3);
}

// The linear forms, BGR10_XR_sRGB and BGRA10_XR_sRGB (their colour codes read and written through Metal's sRGB tables:
// xr_srgb.h) are kept, each sampled through its stand-in's conversion. RGB10A8_2P_XR10_sRGB (551) has no form a pass
// renders in place (its format stand-in renders it); over a surface's two planes it is sampled through its own conversion,
// and one of a descriptor, which holds nothing as Metal's does, through none
TEST(GM20BXr, TheLinearFormsAndTheOneImageSrgbTwinsAreKept)
{
    EXPECT_EQ(gm20b::xrForm(550), gm20b::XrForm::Planes);
    EXPECT_EQ(gm20b::xrForm(552), gm20b::XrForm::Bgra10);
    EXPECT_EQ(gm20b::xrForm(553), gm20b::XrForm::Bgra10Srgb);
    EXPECT_EQ(gm20b::xrForm(554), gm20b::XrForm::Bgr10);
    EXPECT_EQ(gm20b::xrForm(555), gm20b::XrForm::Bgr10Srgb);
    for (uint32_t other : {551u, 80u, 115u})
        EXPECT_EQ(gm20b::xrForm(other), gm20b::XrForm::None);
    EXPECT_STREQ(gm20b::xrConversion(gm20b::XrForm::Planes), "raw.xr.xr_planes_frag");
    EXPECT_STREQ(gm20b::xrConversion(gm20b::XrForm::Bgr10Srgb), "raw.xr.xr_bgr10_srgb_frag");
    EXPECT_STREQ(gm20b::xrConversion(gm20b::XrForm::Bgra10Srgb), "raw.xr.xr_bgra10_srgb_frag");
    EXPECT_EQ(gm20b::xrConversion(gm20b::XrForm::None), nullptr);
    EXPECT_TRUE(gm20b::xrSampled(551) && gm20b::twoPlaneXr(551) && gm20b::twoPlaneXr(550));
    EXPECT_FALSE(gm20b::xrSampled(80) || gm20b::twoPlaneXr(552));
    EXPECT_STREQ(gm20b::xrConversionOf(551), "raw.xr.xr_planes_srgb_frag") << "one of a descriptor keeps its texels";
    EXPECT_STREQ(gm20b::xrConversionOf(551, true), "raw.xr.xr_two_planes_srgb_frag");
    EXPECT_STREQ(gm20b::xrConversionOf(550, true), "raw.xr.xr_two_planes_frag");
}

TEST(GM20BXr, ABlitWithABufferMovesTheTwoPlaneFormsColorWordAlone)
{
    EXPECT_EQ(gm20b::bufferTexelBytes(550, 8), 4u);
    EXPECT_EQ(gm20b::bufferTexelBytes(551, 8), 4u);
    EXPECT_EQ(gm20b::bufferTexelBytes(552, 8), 8u);
    EXPECT_EQ(gm20b::bufferTexelBytes(554, 4), 4u);
    EXPECT_EQ(gm20b::bufferTexelBytes(115, 8), 8u);
}

TEST(GM20BXr, ZeroIsCode384AndBgr10WritesItsTopBits)
{
    expectTexel(clear(gm20b::XrForm::Bgr10, 0, 0, 0, 1), 0xd8060180, 0, 0, 0);
}

TEST(GM20BXr, AWriteRoundsHalfToEvenAndClampsPastTheRange)
{
    // 0.25 * 510 = 127.5 rounds to 128 (code 512), -0.25 to code 256, 2.0 past 1.5 clamps to 1023
    expectTexel(clear(gm20b::XrForm::Bgr10, 0.25f, -0.25f, 2.0f, 1), 0xe00403ff, 0, 0, 0);
}

TEST(GM20BXr, TheStraightRouteRoundsTiesUpInFloatStepsWithTheTopBitsClear)
{
    // 0.25 is 511 and -0.25 is 257 there
    expectTexel(clear(gm20b::XrForm::Bgr10, 0.25f, -0.25f, 2.0f, 1, true), 0x1ff407ff, 0, 0, 0);
}

TEST(GM20BXr, TwoPlaneTexelsHoldTheColorWordAndTheAlphaByte)
{
    expectTexel(clear(gm20b::XrForm::Planes, 0.25f, -0.5f, 1.125f, 0.75f), 0x200207be, 0xbf, 0, 0);
    // the straight route is not the two-plane formats'
    expectTexel(clear(gm20b::XrForm::Planes, 0.25f, -0.5f, 1.125f, 0.75f, true), 0x200207be, 0xbf, 0, 0);
}

TEST(GM20BXr, Bgra10FieldsAreBlueFirstEachCodeInItsTopTenBits)
{
    expectTexel(clear(gm20b::XrForm::Bgra10, 1, 0, -2.0f, 0.5f), 0, 0x6000, 0xdf80, 0x9fc0);
}

TEST(GM20BXr, NaNIsColorCodeZeroAndAlphaZero)
{
    const float nan = std::nanf("");
    expectTexel(clear(gm20b::XrForm::Bgra10, nan, 0.1f, 1.0f, nan), 0xdf80, 0x6cc0, 0x6000, 0x6000);
    expectTexel(clear(gm20b::XrForm::Bgra10, nan, 0.1f, 1.0f, nan, true), 0xdf80, 0x6cc0, 0, 0x6000);
}

// Metal's CPU copies of a shared RGB10A8_2P_XR10 texture move colour words alone: getBytes gives each texel's word in 4
// bytes, replaceRegion writes them and keeps the alpha plane (an M4 Max, m2vf-shapes2 probe/hwref/xr550cpu.swift: the
// words round-trip and a sampled texel keeps alpha 128 / 255 after it, for a 2D, 2D array and 3D texture)
TEST(GM20BXr, TwoPlaneCpuCopiesMoveTheColourWordsAndKeepTheAlpha)
{
    constexpr uint32_t w = 3, h = 2, d = 2;
    std::vector<uint8_t> texels(size_t(w) * h * d * gm20b::kXrTexelBytes);
    for (size_t i = 0; i < texels.size(); i++)
        texels[i] = uint8_t(0xa0 + i);
    const std::vector<uint8_t> before = texels;
    std::vector<uint32_t> words(size_t(w) * h * d);
    for (size_t i = 0; i < words.size(); i++)
        words[i] = 0x10000000u | uint32_t(i * 0x40401);
    const size_t wordRow = w * 4, wordImage = wordRow * h, texelRow = w * 8, texelImage = texelRow * h;
    gm20b::xrWordsIntoTexels(reinterpret_cast<const uint8_t *>(words.data()), wordRow, wordImage, texels.data(),
                             texelRow, texelImage, w, h, d);
    for (size_t i = 0; i < words.size(); i++) {
        uint32_t word, alpha, alphaBefore;
        memcpy(&word, &texels[i * 8], 4);
        memcpy(&alpha, &texels[i * 8 + 4], 4);
        memcpy(&alphaBefore, &before[i * 8 + 4], 4);
        EXPECT_EQ(word, words[i]) << i;
        EXPECT_EQ(alpha, alphaBefore) << i;
    }
    std::vector<uint32_t> back(words.size());
    gm20b::xrWordsFromTexels(texels.data(), texelRow, texelImage, reinterpret_cast<uint8_t *>(back.data()), wordRow,
                             wordImage, w, h, d);
    EXPECT_EQ(back, words);
}

}  // namespace
