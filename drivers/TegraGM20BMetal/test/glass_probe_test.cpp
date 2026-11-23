// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <string>
#include "drivers/TegraGM20BMetal/glass_probe.h"

namespace {

struct Chain {
    struct {
        uint32_t levels;
    } layout;
};

struct FakeTexture {
    uint32_t width = 76, height = 632, format = 80;
    bool blockLinear = true, memoryless = false, arena = false, comptags = false, shadowTarget = false,
         probeWritten = false;
    const Chain *chain = nullptr;
    const void *import = nullptr, *standIn = nullptr, *sampled = nullptr;
};

std::string texture(const FakeTexture &t, uint32_t slot = 3)
{
    char out[96];
    gm20b::glassProbeTexture(out, sizeof out, slot, t);
    return out;
}

}  // namespace

// The record word HW Test's GlassProbe.swift unpacks: counts in their fields, saturating, the mach time's low 40 bits
TEST(GM20BGlassProbe, CommandBufferEntryRoundTrips)
{
    gm20b::GlassCbEntry e;
    e.draws = 118;
    e.encoders = 53;
    e.levels = 7;
    e.ticks = (1ull << 45) + 123456789;
    const gm20b::GlassCbEntry back = gm20b::GlassCbEntry::unpack(e.packed());
    EXPECT_EQ(back.draws, 118u);
    EXPECT_EQ(back.encoders, 53u);
    EXPECT_EQ(back.levels, 7u);
    EXPECT_EQ(back.ticks, 123456789u);
    e.draws = 5000;
    e.encoders = 300;
    e.levels = 64;
    const gm20b::GlassCbEntry full = gm20b::GlassCbEntry::unpack(e.packed());
    EXPECT_EQ(full.draws, 1023u);
    EXPECT_EQ(full.encoders, 255u);
    EXPECT_EQ(full.levels, 63u);
    EXPECT_EQ(full.ticks, 123456789u);
}

TEST(GM20BGlassProbe, TextureNamesItsShapeAndWhetherThisProcessWroteIt)
{
    FakeTexture t;
    EXPECT_EQ(texture(t), "t3:76x632/f80/bl/nw");
    const Chain chain{{7}};
    t.chain = &chain;
    t.probeWritten = true;
    EXPECT_EQ(texture(t), "t3:76x632/f80/bl/L7/w");
    FakeTexture surface;
    surface.blockLinear = false;
    surface.import = &chain;
    surface.shadowTarget = true;
    EXPECT_EQ(texture(surface, 0), "t0:76x632/f80/pitch+imp+sh/nw");
    FakeTexture placed;
    placed.arena = true;
    EXPECT_EQ(texture(placed, 1), "t1:76x632/f80/bl+heap/nw");
}

TEST(GM20BGlassProbe, UniformsCountNonFiniteFloatsAndHalves)
{
    float words[4] = {100.8f, 0.0099f, 0, INFINITY};
    char out[64];
    gm20b::glassProbeUniform(out, sizeof out, 2, 1, reinterpret_cast<const uint8_t *>(words), sizeof words);
    EXPECT_STREQ(out, "u2.1:16/1f1h");
    gm20b::glassProbeUniform(out, sizeof out, 1, 0, nullptr, 272);
    EXPECT_STREQ(out, "u1.0:272/nocpu");
}

TEST(GM20BGlassProbe, EachLineOnceACommandBufferAndGeneration)
{
    const int a = 0, b = 0;
    gm20b::armGlassProbe();
    EXPECT_TRUE(gm20b::glassProbeArmed());
    EXPECT_TRUE(gm20b::glassProbeFirst(&a, "state x"));
    EXPECT_FALSE(gm20b::glassProbeFirst(&a, "state x"));
    EXPECT_TRUE(gm20b::glassProbeFirst(&a, "state y"));
    EXPECT_TRUE(gm20b::glassProbeFirst(&b, "state x"));
    gm20b::armGlassProbe();
    EXPECT_TRUE(gm20b::glassProbeFirst(&b, "state x"));
    gm20b::disarmGlassProbe();
    EXPECT_FALSE(gm20b::glassProbeArmed());
}

TEST(GM20BGlassProbe, BlurPassesNameTheirWeightedTaps)
{
    EXPECT_TRUE(gm20b::glassProbeBlurState("PRGhAXm_Tds4A2Xhf_Isrc"));
    EXPECT_TRUE(gm20b::glassProbeBlurState("PRGhAXm_Tdb2A2Xhf_Isrc"));
    EXPECT_FALSE(gm20b::glassProbeBlurState("PBGRAXmw_TimgA3S1Xhfu_IsrcM0"));
    EXPECT_FALSE(gm20b::glassProbeBlurState(nullptr));
    uint8_t u[80] = {};
    const float offsets[4] = {-0.5f, 0.25f, 0.5f, 0.25f};
    const _Float16 weights[2] = {_Float16(0.5f), _Float16(0.5f)};
    memcpy(u, offsets, sizeof offsets);
    memcpy(u + 64, weights, sizeof weights);
    char out[160];
    gm20b::glassProbeTaps(out, sizeof out, u, sizeof u);
    EXPECT_STREQ(out, "taps -0.5,0.25:0.5 0.5,0.25:0.5");
    gm20b::glassProbeTaps(out, sizeof out, nullptr, 80);
    EXPECT_STREQ(out, "taps -");
}
