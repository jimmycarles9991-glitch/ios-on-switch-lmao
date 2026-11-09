// SPDX-License-Identifier: MIT
// raw/runtime/raw_xr.h: the extended-range formats the raw path keeps, and the codes Metal's Clear stores in them. The texels
// below are an Apple M4 Max's, read back by raw/test/xroracle.mm, which holds xrClearTexel to Metal's clears on this Mac's GPU
// over 755 colors of each linear form by both routes, and pack/xr.metal's stand-in to Metal's reads of every code.
#include "../raw/runtime/raw_xr.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

struct Texel {
    uint32_t w[4];
};

Texel clear(raw::XrForm form, float r, float g, float b, float a, bool straight = false)
{
    const float color[4] = {r, g, b, a};
    Texel t{};
    raw::xrClearTexel(form, color, straight, t.w);
    return t;
}

void expectTexel(const Texel &t, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    EXPECT_EQ(t.w[0], w0);
    EXPECT_EQ(t.w[1], w1);
    EXPECT_EQ(t.w[2], w2);
    EXPECT_EQ(t.w[3], w3);
}

TEST(RawXr, TheLinearFormsAreKeptAndTheirSrgbTwinsAreNot)
{
    EXPECT_EQ(raw::xrForm(550), raw::XrForm::Planes);
    EXPECT_EQ(raw::xrForm(552), raw::XrForm::Bgra10);
    EXPECT_EQ(raw::xrForm(554), raw::XrForm::Bgr10);
    for (uint32_t srgb : {551u, 553u, 555u, 80u, 115u})
        EXPECT_EQ(raw::xrForm(srgb), raw::XrForm::None);
    EXPECT_STREQ(raw::xrConversion(raw::XrForm::Planes), "raw.xr.xr_planes_frag");
    EXPECT_EQ(raw::xrConversion(raw::XrForm::None), nullptr);
}

TEST(RawXr, ABlitWithABufferMovesTheTwoPlaneFormsColorWordAlone)
{
    EXPECT_EQ(raw::bufferTexelBytes(550, 8), 4u);
    EXPECT_EQ(raw::bufferTexelBytes(552, 8), 8u);
    EXPECT_EQ(raw::bufferTexelBytes(554, 4), 4u);
    EXPECT_EQ(raw::bufferTexelBytes(115, 8), 8u);
}

TEST(RawXr, ZeroIsCode384AndBgr10WritesItsTopBits)
{
    expectTexel(clear(raw::XrForm::Bgr10, 0, 0, 0, 1), 0xd8060180, 0, 0, 0);
}

TEST(RawXr, AWriteRoundsHalfToEvenAndClampsPastTheRange)
{
    // 0.25 * 510 = 127.5 rounds to 128 (code 512), -0.25 to code 256, 2.0 past 1.5 clamps to 1023
    expectTexel(clear(raw::XrForm::Bgr10, 0.25f, -0.25f, 2.0f, 1), 0xe00403ff, 0, 0, 0);
}

TEST(RawXr, TheStraightRouteRoundsTiesUpInFloatStepsWithTheTopBitsClear)
{
    // 0.25 is 511 and -0.25 is 257 there
    expectTexel(clear(raw::XrForm::Bgr10, 0.25f, -0.25f, 2.0f, 1, true), 0x1ff407ff, 0, 0, 0);
}

TEST(RawXr, TwoPlaneTexelsHoldTheColorWordAndTheAlphaByte)
{
    expectTexel(clear(raw::XrForm::Planes, 0.25f, -0.5f, 1.125f, 0.75f), 0x200207be, 0xbf, 0, 0);
    // the straight route is not the two-plane formats'
    expectTexel(clear(raw::XrForm::Planes, 0.25f, -0.5f, 1.125f, 0.75f, true), 0x200207be, 0xbf, 0, 0);
}

TEST(RawXr, Bgra10FieldsAreBlueFirstEachCodeInItsTopTenBits)
{
    expectTexel(clear(raw::XrForm::Bgra10, 1, 0, -2.0f, 0.5f), 0, 0x6000, 0xdf80, 0x9fc0);
}

TEST(RawXr, NaNIsColorCodeZeroAndAlphaZero)
{
    const float nan = std::nanf("");
    expectTexel(clear(raw::XrForm::Bgra10, nan, 0.1f, 1.0f, nan), 0xdf80, 0x6cc0, 0x6000, 0x6000);
    expectTexel(clear(raw::XrForm::Bgra10, nan, 0.1f, 1.0f, nan, true), 0xdf80, 0x6cc0, 0, 0x6000);
}

}  // namespace
