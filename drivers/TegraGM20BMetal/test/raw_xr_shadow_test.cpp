// SPDX-License-Identifier: MIT
// raw/runtime/raw_xr_shadow.h: which colours of a pass hold a half shadow (m2v's runtime render/extended_shadow.rs rule over
// the pass's whole list of draws), what a Clear puts in the shadow and whether its codes hold it, and the tile value the
// shadow is filled with from the codes (raw/test/xrtileoracle.mm holds pack/xr.metal's xr_tile_* states and this model to an
// M4 Max's tile, every code of BGR10_XR and BGRA10_XR)
#include "../raw/runtime/raw_xr_shadow.h"

#include <gtest/gtest.h>

namespace {

TEST(RawXrShadow, AColourReadAfterADrawOfThePassWroteItHoldsAShadow)
{
    raw::XrShadowPass p;
    p.eligible = 0b11;
    p.draw(0, 0b01);
    p.draw(0b10, 0b10);
    EXPECT_EQ(p.held(), 0);
    p.draw(0b01, 0b01);
    EXPECT_EQ(p.held(), 0b01);
    p.draw(0b10, 0);
    EXPECT_EQ(p.held(), 0b11);
    p.blocked = true;
    EXPECT_EQ(p.held(), 0);
}

TEST(RawXrShadow, AClearItsCodesCannotHoldIsReadThroughAShadowFromTheFirstDraw)
{
    raw::XrShadowPass p;
    p.eligible = 0b101;
    p.clearedOff = 0b100;
    p.draw(0b111, 0);
    EXPECT_EQ(p.held(), 0b100);
    raw::XrShadowPass other;
    other.eligible = 0b001;
    other.draw(0b011, 0b011);
    other.draw(0b011, 0b011);
    EXPECT_EQ(other.held(), 0b001);
}

TEST(RawXrShadow, AClearIsHeldWhereItsCodeReadsBackItsHalf)
{
    using raw::XrForm;
    const double held[4] = {0.5, 0.3, 0.0, 1.0}, off[4] = {0.123, 0.5, 0.5, 1.0}, alphaOff[4] = {0.5, 0.5, 0.5, 0.3};
    for (const XrForm f : {XrForm::Bgr10, XrForm::Bgra10, XrForm::Planes}) {
        EXPECT_TRUE(raw::xrClearHeldByCodes(f, held));
        EXPECT_FALSE(raw::xrClearHeldByCodes(f, off));
    }
    EXPECT_TRUE(raw::xrClearHeldByCodes(XrForm::Bgr10, alphaOff));
    EXPECT_FALSE(raw::xrClearHeldByCodes(XrForm::Planes, alphaOff));
    // the codes of Metal's Clear (raw_xr.h, an M4 Max's) read back as the tile's halves exactly where they are held
    for (double v = -0.75; v <= 1.25; v += 0.0137) {
        const double color[4] = {v, 0.5, 0.5, 1.0};
        const float clear[4] = {float(v), 0.5f, 0.5f, 1.0f};
        uint32_t texel[4];
        raw::xrClearTexel(XrForm::Bgr10, clear, false, texel);
        float tile[4];
        raw::xrClearTile(XrForm::Bgr10, color, tile);
        const float back = raw::xrTileValue(XrForm::Bgr10, texel[0] >> 20 & 0x3ff, false);
        EXPECT_EQ(raw::xrClearHeldByCodes(XrForm::Bgr10, color), back == tile[0]) << v;
        EXPECT_EQ(tile[3], 1.0f);
    }
}

TEST(RawXrShadow, TheTileHoldsNoColourCodePast1022)
{
    using raw::XrForm;
    EXPECT_EQ(raw::xrTileValue(XrForm::Bgr10, 1023, false), raw::xrTileValue(XrForm::Bgr10, 1022, false));
    EXPECT_EQ(raw::xrTileValue(XrForm::Bgr10, 1022, false), float(_Float16(638.0 / 510.0)));
    EXPECT_EQ(raw::xrTileValue(XrForm::Bgra10, 0, true), 0.0f);
    EXPECT_EQ(raw::xrTileValue(XrForm::Bgra10, 1023, true), 1.0f);
    EXPECT_EQ(raw::xrTileValue(XrForm::Planes, 255, true), 1.0f);
    EXPECT_EQ(raw::xrTileValue(XrForm::Bgr10, 0, true), 1.0f);
    EXPECT_EQ(raw::xrTileValue(XrForm::Bgr10, 0, false), float(_Float16(-384.0 / 510.0)));
}

TEST(RawXrShadow, TheVariantNamesItsColoursAndTheFormsThatHoldOne)
{
    EXPECT_EQ(raw::xrShadowVariant(0), "");
    EXPECT_EQ(raw::xrShadowColours("key | tests=2" + raw::xrShadowVariant(5) + " | xrs=3"), 5);
    EXPECT_EQ(raw::xrShadowColours("key | xrs=3"), 0);
    EXPECT_EQ(raw::xrShadowForm(554, false), raw::XrForm::Bgr10);
    EXPECT_EQ(raw::xrShadowForm(552, false), raw::XrForm::Bgra10);
    EXPECT_EQ(raw::xrShadowForm(550, false), raw::XrForm::Planes);
    EXPECT_EQ(raw::xrShadowForm(550, true), raw::XrForm::None);
    EXPECT_EQ(raw::xrShadowForm(555, false), raw::XrForm::None);
    EXPECT_STREQ(raw::xrTileState(raw::XrForm::Bgra10), "raw.xr.xr_tile_bgra10_frag");
}

}  // namespace
