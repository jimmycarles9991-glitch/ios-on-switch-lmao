// SPDX-License-Identifier: MIT
// raw/runtime/raw_fetch_rungs.h: where the ROP's blend and iterated blend may stand for an in-shader blend.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_fetch_rungs.h"

namespace {

raw::FixedBlendAsk screenLayer()
{
    raw::FixedBlendAsk a;
    a.format = 80;
    a.standardFactors = true;
    a.eightBitSources = true;
    return a;
}

TEST(RawFetchRungs, AnEightBitLayerOnAScreenTargetTakesTheRop)
{
    EXPECT_TRUE(raw::fixedBlendAcceptable(screenLayer()));
    raw::FixedBlendAsk a = screenLayer();
    a.format = 70;
    EXPECT_TRUE(raw::fixedBlendAcceptable(a));
}

TEST(RawFetchRungs, ComputedColourTakesItTwoLayersDeep)
{
    raw::FixedBlendAsk a = screenLayer();
    a.eightBitSources = false;
    a.computedLayers = 2;
    EXPECT_TRUE(raw::fixedBlendAcceptable(a));
    a.computedLayers = 3;
    EXPECT_FALSE(raw::fixedBlendAcceptable(a)) << "4 codes off by eight layers (UNITS.md stack table)";
}

TEST(RawFetchRungs, EachFormatTheFormatArmsJudgeTakesTheRop)
{
    for (uint16_t format : {uint16_t(115), uint16_t(81), uint16_t(71)}) {
        raw::FixedBlendAsk a = screenLayer();
        a.format = format;
        EXPECT_EQ(raw::fixedBlendAcceptable(a), raw::fixedBlendProven()) << format;
    }
}

TEST(RawFetchRungs, AnSrgbTargetTakesEightBitSourcesTwoLayersDeepAsComputedColour)
{
    for (uint16_t format : {uint16_t(81), uint16_t(71)}) {
        raw::FixedBlendAsk a = screenLayer();
        a.format = format;
        a.computedLayers = 2;
        EXPECT_EQ(raw::fixedBlendAcceptable(a), raw::fixedBlendProven()) << format;
        a.computedLayers = 3;
        EXPECT_FALSE(raw::fixedBlendAcceptable(a)) << format << ": 3 codes off Metal by eight layers";
    }
    raw::FixedBlendAsk a = screenLayer();
    a.computedLayers = 8;
    EXPECT_EQ(raw::fixedBlendAcceptable(a), raw::fixedBlendProven()) << "BGRA8's 8-bit sources at any depth";
}

TEST(RawFetchRungs, DataTargetsKeepTheExactBlend)
{
    for (uint16_t format : {uint16_t(90), uint16_t(110), uint16_t(72), uint16_t(550), uint16_t(552), uint16_t(554)}) {
        raw::FixedBlendAsk a = screenLayer();
        a.format = format;
        EXPECT_FALSE(raw::fixedBlendAcceptable(a)) << format;
    }
    raw::FixedBlendAsk a = screenLayer();
    a.readable = true;
    EXPECT_FALSE(raw::fixedBlendAcceptable(a));
    a = screenLayer();
    a.accumulates = true;
    EXPECT_FALSE(raw::fixedBlendAcceptable(a));
    a = screenLayer();
    a.standardFactors = false;
    EXPECT_FALSE(raw::fixedBlendAcceptable(a));
}

TEST(RawFetchRungs, IteratedBlendTakesTheProgramsWhosePassesBeatTheInterlockOnOneTarget)
{
    raw::IteratedAsk a;
    a.candidate = true;
    a.colourTargets = 1;
    a.programWords = 2;
    a.program = raw::iteratedProgramName("mpy");
    EXPECT_TRUE(raw::iteratedBlendUsable(a));
    a.colourTargets = 2;
    EXPECT_FALSE(raw::iteratedBlendUsable(a)) << "the unit refuses a second colour target (0x59)";
    a.colourTargets = 1;
    for (const char *slower : {"slt", "dso", "dlc", "vcm", "vcm.rows", "lum", "cli", "hue"}) {
        a.program = raw::iteratedProgramName(slower);
        EXPECT_FALSE(raw::iteratedBlendUsable(a)) << slower << " costs more passes than the interlock";
    }
    EXPECT_NE(raw::iteratedProgramName("dso"), raw::iteratedProgramName("pld"));
}

}  // namespace
