// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchVICComposeProbe.h"

namespace {
using namespace switch_vic_compose_probe;

// An engine that writes what model m says for each case, through the destination's layout.
std::vector<uint8_t> run(const Case &c, Model m)
{
    std::vector<uint8_t> destination(kBufferBytes, kPoison);
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++) {
            Rgba want;
            if (!expected(c, m, x, y, want))
                continue;
            const uint32_t v = packed(want);
            memcpy(destination.data() + pixelOffset(c.blockLinear, x, y), &v, 4);
        }
    return destination;
}

SwitchVICComposeResult compared(const Case &c, const std::vector<uint8_t> &bytes)
{
    SwitchVICComposeResult r = {};
    compare(bytes.data(), c, &r);
    return r;
}

TEST(VICComposeProbe, EveryCaseBuildsWithinOneJob)
{
    for (uint32_t i = 0; i < SWITCH_VIC_COMPOSE_PROBE_CASES; i++) {
        TegraVICJob job;
        EXPECT_EQ(build(kCases[i], 0x10000000, 0x20000000, 0x1000, 0x2000, &job), nullptr) << i;
        EXPECT_LE(job.count, uint32_t(TEGRA_VIC_MAX_METHODS)) << i;
    }
}

TEST(VICComposeProbe, TheTweakedCasesChangeOnlyTheBlendWordsOrTheFillMode)
{
    TegraVICJob plain, zero, composited;
    ASSERT_EQ(build(kCases[3], 0x10000000, 0x20000000, 0x1000, 0x2000, &plain), nullptr);
    ASSERT_EQ(build(kCases[10], 0x10000000, 0x20000000, 0x1000, 0x2000, &zero), nullptr);
    uint8_t empty[16] = {};
    for (uint32_t s = 0; s < 2; s++) {
        const uint32_t at = TEGRA_VIC_SLOTS + s * TEGRA_VIC_SLOT_BYTES + TEGRA_VIC_SLOT_BLEND;
        EXPECT_NE(memcmp(plain.config + at, empty, 16), 0);
        EXPECT_EQ(memcmp(zero.config + at, empty, 16), 0);
        memset(plain.config + at, 0, 16);
    }
    EXPECT_EQ(memcmp(plain.config, zero.config, TEGRA_VIC_CONFIG_BYTES), 0);
    ASSERT_EQ(build(kCases[11], 0x10000000, 0x20000000, 0x1000, 0x2000, &composited), nullptr);
    EXPECT_EQ(TEGRA_VIC_GET(composited.config, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE), uint64_t(kAlphaFillComposited));
}

TEST(VICComposeProbe, AnEngineDoingWhatWasProgrammedIsCleanInEveryCase)
{
    SwitchVICComposeProbeReply reply = {};
    reply.count = SWITCH_VIC_COMPOSE_PROBE_CASES;
    for (uint32_t i = 0; i < reply.count; i++) {
        reply.results[i] = compared(kCases[i], run(kCases[i], kProgrammed));
        EXPECT_TRUE(reply.results[i].models & (1u << kProgrammed)) << i;
        EXPECT_EQ(reply.results[i].mismatches, 0u) << i;
    }
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 1u);
    EXPECT_EQ(reply.cleanCases, (1u << SWITCH_VIC_COMPOSE_PROBE_CASES) - 1);
}

TEST(VICComposeProbe, EachWrongReadingIsNamedWhereItDiffers)
{
    const Case &premultiplied = kCases[5], &straight = kCases[6], &global = kCases[7], &order = kCases[3];
    const auto models = [](const Case &c, Model m) {
        return compared(c, run(c, m)).models;
    };
    for (const Case *c : {&premultiplied, &straight, &global}) {
        EXPECT_FALSE(models(*c, kTopReplaces) & (1u << kProgrammed));
        EXPECT_TRUE(models(*c, kTopReplaces) & (1u << kTopReplaces));
        EXPECT_FALSE(models(*c, kTopMissing) & (1u << kProgrammed));
        EXPECT_FALSE(models(*c, kNothing) & (1u << kProgrammed));
    }
    EXPECT_FALSE(models(premultiplied, kAsStraight) & (1u << kProgrammed));
    EXPECT_FALSE(models(straight, kAsPremultiplied) & (1u << kProgrammed));
    EXPECT_TRUE(models(premultiplied, kProgrammed) & (1u << kAsPremultiplied));
    EXPECT_FALSE(models(order, kTopMissing) & (1u << kProgrammed));
    EXPECT_FALSE(models(kCases[4], kTopMissing) & (1u << kProgrammed));
}

TEST(VICComposeProbe, TheFirstWrongPixelAndStrayWritesAreReported)
{
    const Case &c = kCases[3];
    auto bytes = run(c, kProgrammed);
    const uint64_t at = pixelOffset(false, 20, 10);
    bytes[at] ^= 0x40;
    bytes[kBufferBytes - 1] = 0;
    const auto r = compared(c, bytes);
    EXPECT_EQ(r.mismatches, 1u);
    EXPECT_EQ(r.firstX, 20u);
    EXPECT_EQ(r.firstY, 10u);
    EXPECT_EQ(r.outsideWritten, 1u);
    EXPECT_FALSE(clean(r));
}

TEST(VICComposeProbe, ThePitchLinearPictureDoesNotReadBackAsBlocks)
{
    Case asPitch = kCases[9];
    asPitch.blockLinear = false;
    const auto bytes = run(asPitch, kProgrammed);
    EXPECT_GT(compared(kCases[9], bytes).mismatches, 0u);
}

TEST(VICComposeProbe, TheControlLeavesEverythingOutsideItsRectanglePoisoned)
{
    const Case &c = kCases[0];
    auto bytes = run(c, kProgrammed);
    EXPECT_EQ(compared(c, bytes).mismatches, 0u);
    const uint32_t black = 0xff000000u;
    memcpy(bytes.data() + pixelOffset(false, 40, 20), &black, 4);
    EXPECT_EQ(compared(c, bytes).mismatches, 1u);
}

TEST(VICComposeProbe, VideoColourIsTheEnginesBt601VideoRange)
{
    const Rgba v = videoColour();
    EXPECT_NEAR(double(v.c[0]), 1.164 * (kLuma - 16) + 1.596 * (kCr - 128), 2.5);
    EXPECT_NEAR(double(v.c[2]), 1.164 * (kLuma - 16) + 2.018 * (kCb - 128), 2.5);
}
}  // namespace
