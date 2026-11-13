// SPDX-License-Identifier: MIT
// raw/runtime/raw_constant_colour.h: which fills the ROP's constant colour may draw, once the rule holds.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_constant_colour.h"

namespace {

raw::FillDraw plainFill()
{
    raw::FillDraw d;
    d.fillState = true;
    d.oneTarget = true;
    d.oneColour = true;
    return d;
}

TEST(RawConstantColour, OnlyAPlainOneColourFillQualifies)
{
    EXPECT_EQ(raw::constantColourFill(plainFill()), raw::ropConstantColour());
    raw::FillDraw d = plainFill();
    d.fillState = false;
    EXPECT_FALSE(raw::constantColourFill(d));
    d = plainFill();
    d.samplesTexture = true;
    EXPECT_FALSE(raw::constantColourFill(d));
    d = plainFill();
    d.shapesCoverage = true;
    EXPECT_FALSE(raw::constantColourFill(d));
    d = plainFill();
    d.oneTarget = false;
    EXPECT_FALSE(raw::constantColourFill(d));
    d = plainFill();
    d.oneColour = false;
    EXPECT_FALSE(raw::constantColourFill(d));
}

TEST(RawConstantColour, TheRuleWaitsForTheDevice)
{
    EXPECT_FALSE(raw::ropConstantColour()) << "gpu.x1.constant_colour has not run on the chip";
}

}  // namespace
