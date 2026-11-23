// SPDX-License-Identifier: MIT
// fetch_rungs.h: where the ROP's blend and iterated blend may stand for an in-shader blend.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/fetch_rungs.h"

namespace {

gm20b::FixedBlendAsk screenLayer()
{
    gm20b::FixedBlendAsk a;
    a.format = 80;
    a.standardFactors = true;
    a.eightBitSources = true;
    return a;
}

TEST(GM20BFetchRungs, AnEightBitLayerOnAScreenTargetTakesTheRop)
{
    EXPECT_TRUE(gm20b::fixedBlendAcceptable(screenLayer()));
    gm20b::FixedBlendAsk a = screenLayer();
    a.format = 70;
    EXPECT_TRUE(gm20b::fixedBlendAcceptable(a));
}

TEST(GM20BFetchRungs, ComputedColourTakesItTwoLayersDeep)
{
    gm20b::FixedBlendAsk a = screenLayer();
    a.eightBitSources = false;
    a.computedLayers = 2;
    EXPECT_TRUE(gm20b::fixedBlendAcceptable(a));
    a.computedLayers = 3;
    EXPECT_FALSE(gm20b::fixedBlendAcceptable(a)) << "4 codes off by eight layers (UNITS.md stack table)";
}

TEST(GM20BFetchRungs, EachFormatTheFormatArmsJudgeTakesTheRop)
{
    for (uint16_t format : {uint16_t(115), uint16_t(81), uint16_t(71)}) {
        gm20b::FixedBlendAsk a = screenLayer();
        a.format = format;
        EXPECT_EQ(gm20b::fixedBlendAcceptable(a), gm20b::fixedBlendProven()) << format;
    }
}

TEST(GM20BFetchRungs, AnSrgbTargetTakesEightBitSourcesTwoLayersDeepAsComputedColour)
{
    for (uint16_t format : {uint16_t(81), uint16_t(71)}) {
        gm20b::FixedBlendAsk a = screenLayer();
        a.format = format;
        a.computedLayers = 2;
        EXPECT_EQ(gm20b::fixedBlendAcceptable(a), gm20b::fixedBlendProven()) << format;
        a.computedLayers = 3;
        EXPECT_FALSE(gm20b::fixedBlendAcceptable(a)) << format << ": 3 codes off Metal by eight layers";
    }
    gm20b::FixedBlendAsk a = screenLayer();
    a.computedLayers = 8;
    EXPECT_EQ(gm20b::fixedBlendAcceptable(a), gm20b::fixedBlendProven()) << "BGRA8's 8-bit sources at any depth";
}

TEST(GM20BFetchRungs, DataTargetsKeepTheExactBlend)
{
    for (uint16_t format : {uint16_t(90), uint16_t(110), uint16_t(72), uint16_t(550), uint16_t(552), uint16_t(554)}) {
        gm20b::FixedBlendAsk a = screenLayer();
        a.format = format;
        EXPECT_FALSE(gm20b::fixedBlendAcceptable(a)) << format;
    }
    gm20b::FixedBlendAsk a = screenLayer();
    a.readable = true;
    EXPECT_FALSE(gm20b::fixedBlendAcceptable(a));
    a = screenLayer();
    a.accumulates = true;
    EXPECT_FALSE(gm20b::fixedBlendAcceptable(a));
    a = screenLayer();
    a.standardFactors = false;
    EXPECT_FALSE(gm20b::fixedBlendAcceptable(a));
}

TEST(GM20BFetchRungs, IteratedBlendTakesTheProgramsWhosePassesBeatTheInterlockOnOneTarget)
{
    gm20b::IteratedAsk a;
    a.candidate = true;
    a.colourTargets = 1;
    a.programWords = 2;
    a.program = gm20b::iteratedProgramName("mpy");
    EXPECT_TRUE(gm20b::iteratedBlendUsable(a));
    a.colourTargets = 2;
    EXPECT_FALSE(gm20b::iteratedBlendUsable(a)) << "the unit refuses a second colour target (0x59)";
    a.colourTargets = 1;
    for (const char *slower : {"slt", "dso", "dlc", "vcm", "vcm.rows", "lum", "cli", "hue"}) {
        a.program = gm20b::iteratedProgramName(slower);
        EXPECT_FALSE(gm20b::iteratedBlendUsable(a)) << slower << " costs more passes than the interlock";
    }
    EXPECT_NE(gm20b::iteratedProgramName("dso"), gm20b::iteratedProgramName("pld"));
}

}  // namespace
