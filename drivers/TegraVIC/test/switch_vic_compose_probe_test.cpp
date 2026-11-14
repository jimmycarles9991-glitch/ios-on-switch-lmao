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

uint32_t to255(uint64_t v10)
{
    return uint32_t((v10 * 255 + 511) / 1023);
}

uint32_t times(uint32_t a255, uint32_t b255)
{
    return (a255 * b255 + 127) / 255;
}

// VIC 4.0 as boot 21's probe read it, from the job's registers: under AlphaFillMode COMPOSITED each slot blends by its
// select codes (VIC 4.1's: colour K1 or K1 x source alpha over 1 - K1 x source alpha, or K1); under any other mode
// each slot is laid over by its own alpha and its blend words and K1 do nothing. A code it does not know gives 0.
std::vector<uint8_t> boot21Engine(const Case &c, const TegraVICJob &job)
{
    std::vector<uint8_t> destination(kBufferBytes, kPoison);
    const uint8_t *cfg = job.config;
    const bool composited =
        TEGRA_VIC_GET(cfg, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE) == uint64_t(TEGRA_VIC_ALPHA_COMPOSITED);
    const uint32_t count = c.slots ? c.slots : 1;
    for (uint64_t y = TEGRA_VIC_GET(cfg, TEGRA_VIC_OUTPUT, OUT_TARGET_TOP);
         y <= TEGRA_VIC_GET(cfg, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM); y++)
        for (uint64_t x = TEGRA_VIC_GET(cfg, TEGRA_VIC_OUTPUT, OUT_TARGET_LEFT);
             x <= TEGRA_VIC_GET(cfg, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT); x++) {
            Rgba out = {{0, 0, 0, 255}};
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t slot = TEGRA_VIC_SLOTS + i * TEGRA_VIC_SLOT_BYTES, blend = slot + TEGRA_VIC_SLOT_BLEND;
                const uint64_t left = TEGRA_VIC_GET(cfg, slot, SLOT_DEST_LEFT),
                               top = TEGRA_VIC_GET(cfg, slot, SLOT_DEST_TOP);
                const uint64_t right = TEGRA_VIC_GET(cfg, slot, SLOT_DEST_RIGHT);
                const uint64_t bottom = TEGRA_VIC_GET(cfg, slot, SLOT_DEST_BOTTOM);
                if (!TEGRA_VIC_GET(cfg, slot, SLOT_ENABLE) || x < left || x > right || y < top || y > bottom)
                    continue;
                const Placed &p = c.slot[i];
                const Rgba s = stored(p.source, uint32_t((x - left) * sourceWidth(p.source) / (right - left + 1)),
                                      uint32_t((y - top) * sourceHeight(p.source) / (bottom - top + 1)));
                const uint32_t a = TEGRA_VIC_GET(cfg, slot, SLOT_CONSTANT_ALPHA)
                                       ? to255(TEGRA_VIC_GET(cfg, slot, SLOT_PLANAR_ALPHA))
                                       : s.c[3];
                uint32_t srcFactor = a, dstFactor = 255 - a;
                if (composited) {
                    const uint32_t k1 = to255(TEGRA_VIC_GET(cfg, blend, SLOT_BLEND_K1)), k1a = times(k1, a);
                    const uint64_t src = TEGRA_VIC_GET(cfg, blend, SLOT_BLEND_SRC_C);
                    const uint64_t dst = TEGRA_VIC_GET(cfg, blend, SLOT_BLEND_DST_C);
                    srcFactor = src == TEGRA_VIC_SRC_C_K1 ? k1 : src == TEGRA_VIC_SRC_C_K1_SRC_ALPHA ? k1a : 0;
                    dstFactor = dst == TEGRA_VIC_DST_C_ONE_MINUS_K1_SRC_ALPHA ? 255 - k1a : dst == 0 ? k1 : 0;
                }
                for (uint32_t ch = 0; ch < 3; ch++)
                    out.c[ch] = over(s.c[ch], out.c[ch], srcFactor, dstFactor);
                if (!c.slots)
                    out.c[3] = s.c[3];
            }
            const uint32_t v = packed(out);
            memcpy(destination.data() + pixelOffset(c.blockLinear, uint32_t(x), uint32_t(y)), &v, 4);
        }
    return destination;
}

SwitchVICComposeProbeReply onBoot21Engine()
{
    SwitchVICComposeProbeReply reply = {};
    for (uint32_t i = 0; i < SWITCH_VIC_COMPOSE_PROBE_CASES; i++, reply.count++) {
        TegraVICJob job;
        EXPECT_EQ(build(kCases[i], 0x10000000, 0x20000000, 0x1000, 0x2000, &job), nullptr) << i;
        reply.results[i] = compared(kCases[i], boot21Engine(kCases[i], job));
    }
    summarise(&reply);
    return reply;
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
    TegraVICJob plain, zero, opaqueFill;
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
    EXPECT_EQ(TEGRA_VIC_GET(plain.config, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE), uint64_t(TEGRA_VIC_ALPHA_COMPOSITED));
    ASSERT_EQ(build(kCases[11], 0x10000000, 0x20000000, 0x1000, 0x2000, &opaqueFill), nullptr);
    EXPECT_EQ(TEGRA_VIC_GET(opaqueFill.config, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE),
              uint64_t(TEGRA_VIC_ALPHA_OPAQUE));
}

// Boot 21's readings come back on its engine: the OPAQUE-fill case reads as straight (premultiplied_models=10 there)
// and the zero blend words draw nothing under COMPOSITED; every judged case the builder makes is clean.
TEST(VICComposeProbe, OnBoot21sEngineEveryJudgedCaseIsClean)
{
    const SwitchVICComposeProbeReply reply = onBoot21Engine();
    EXPECT_EQ(reply.controlsClean, 1u);
    EXPECT_EQ(reply.cleanCases, (1u << 10) - 1);
    EXPECT_EQ(reply.results[11].models, 1u << kAsStraight);
    EXPECT_TRUE(reply.results[10].models & (1u << kNothing));
    for (uint32_t i = 0; i < 10; i++)
        EXPECT_EQ(reply.results[i].mismatches, 0u) << i << " max " << reply.results[i].maxError;
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
