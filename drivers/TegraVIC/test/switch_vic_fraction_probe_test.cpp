// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchVICFractionProbe.h"
#include "vic_model.h"

namespace {
using namespace switch_vic_fraction_probe;

constexpr uint64_t kSrcIova = 0x40000000, kDstIova = 0x40100000;

TegraVICJob built(const Case &c)
{
    const TegraVICImage in = image(kSrcIova, false), out = image(kDstIova, true);
    const TegraVICOp op = operation(c);
    TegraVICJob job = {};
    EXPECT_EQ(tegra_vic_build(&in, &out, &op, &kTegraVICTraits, 0x1000, 0x2000, &job), nullptr);
    return job;
}

enum class Offsets {
    Interpolated,
    Truncated,
    Rounded,
};

enum class Alpha {
    Programmed,  // as the job's AlphaFillMode says
    Source,
    Opaque,
};

uint32_t channel(const std::vector<uint8_t> &source, uint32_t x, uint32_t y, uint32_t shift)
{
    uint32_t p = 0;
    memcpy(&p, source.data() + uint64_t(y) * kSourcePitch + 4ull * x, 4);
    return (p >> shift) & 0xff;
}

enum class Dimensions {
    Kept,
    Truncated,  // the rectangle's width and height dropped to whole pixels
};

// An engine that reads the source at the job's 16.16 rectangle, each output pixel's centre scaled into it, 2-tap in the
// 10-bit pipeline (in << 2, out >> 2, truncating), with the offset kept, truncated or rounded, and alpha as told.
std::vector<uint8_t> engine(const Case &c, Offsets offsets, Alpha alpha, Dimensions dimensions = Dimensions::Kept)
{
    std::vector<uint8_t> source(kBufferBytes), destination(kBufferBytes, kPoison);
    fillSource(source.data(), c);
    const TegraVICOp op = operation(c);
    const TegraVICJob job = built(c);
    const auto at = [offsets](uint64_t fixed) {
        if (offsets == Offsets::Truncated)
            return fixed & ~0xffffull;
        if (offsets == Offsets::Rounded)
            return (fixed + 0x8000) & ~0xffffull;
        return fixed;
    };
    for (uint32_t y = 0; y < kSize; y++) {
        for (uint32_t x = 0; x < kSize; x++) {
            const uint64_t mask = dimensions == Dimensions::Truncated ? ~0xffffull : ~0ull;
            const auto scaled = [](uint64_t origin, uint64_t size, uint32_t i) {
                return origin + ((2 * i + 1) * size - (uint64_t(kSize) << 16)) / (2 * kSize);
            };
            const uint64_t sx = at(scaled(op.srcX, op.srcW & mask, x)), sy = at(scaled(op.srcY, op.srcH & mask, y));
            const uint32_t x0 = uint32_t(sx >> 16), y0 = uint32_t(sy >> 16);
            const uint64_t fx = op.nearest ? 0 : sx & 0xffff, fy = op.nearest ? 0 : sy & 0xffff;
            uint32_t out = 0;
            for (uint32_t shift = 0; shift < 32; shift += 8) {
                const uint64_t a = channel(source, x0, y0, shift) << 2, b = channel(source, x0 + 1, y0, shift) << 2;
                const uint64_t d = channel(source, x0, y0 + 1, shift) << 2;
                const uint64_t e = channel(source, x0 + 1, y0 + 1, shift) << 2;
                const uint64_t top = a * (65536 - fx) + b * fx, bottom = d * (65536 - fx) + e * fx;
                const uint64_t v10 = (top * (65536 - fy) + bottom * fy) >> 32;
                out |= uint32_t(v10 >> 2) << shift;
            }
            if (alpha == Alpha::Opaque)
                out |= 0xffu << 24;
            if (alpha == Alpha::Programmed &&
                TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE) == TEGRA_VIC_ALPHA_BACKGROUND)
                out = (out & 0xffffffu) |
                      uint32_t(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA) >> 2) << 24;
            memcpy(destination.data() + uint64_t(y) * kDestinationPitch + 4ull * x, &out, 4);
        }
    }
    return destination;
}

SwitchVICFractionResult judge(const Case &c, const std::vector<uint8_t> &destination)
{
    SwitchVICFractionResult r = {};
    compare(destination.data(), c, &r);
    return r;
}
}  // namespace

TEST(SwitchVICFractionProbe, EveryCaseBuildsWithTheShippedTraitsAndKeepsItsFraction)
{
    for (const Case &c : kCases) {
        const TegraVICOp op = operation(c);
        const TegraVICJob job = built(c);
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_LEFT), op.srcX);
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_TOP), op.srcY);
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_RIGHT), op.srcX + op.srcW - 0x10000);
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_BOTTOM), op.srcY + op.srcH - 0x10000);
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS, SLOT_FILTER_X),
                  op.nearest ? uint64_t(TEGRA_VIC_FILTER_1TAP) : uint64_t(TEGRA_VIC_FILTER_2TAP));
        const bool overridden = c.mode == kBackgroundAlpha;
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE),
                  uint64_t(overridden ? TEGRA_VIC_ALPHA_BACKGROUND : TEGRA_VIC_ALPHA_SOURCE_STREAM));
        if (overridden)
            EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA),
                      kOverride << 2 | kOverride >> 6);
    }
}

// The alpha cases through tegra_vic_build on vic_model.h (1-tap, 1:1): the override reaches every pixel's alpha and
// SOURCE_STREAM keeps the source's.
TEST(SwitchVICFractionProbe, TheAlphaCasesAreCleanOnTheModel)
{
    for (const Case &c : kCases) {
        if (c.mode != kSourceAlpha && c.mode != kBackgroundAlpha)
            continue;
        std::vector<uint8_t> source(kBufferBytes), destination(kBufferBytes, kPoison);
        fillSource(source.data(), c);
        VicModel model = {};
        model.region[0] = {kSrcIova, source.data(), source.size()};
        model.region[1] = {kDstIova, destination.data(), destination.size()};
        model.regions = 2;
        model.flips = VIC_MODEL_FLIP_SURFACE;
        const TegraVICJob job = built(c);
        vic_model_run(&model, &job);
        EXPECT_EQ(model.faults + model.clipped, 0u) << c.mode;
        const SwitchVICFractionResult r = judge(c, destination);
        EXPECT_TRUE(clean(r)) << c.mode << ": " << r.wrong[SWITCH_VIC_FRACTION_INTERPOLATED] << " wrong";
    }
}

TEST(SwitchVICFractionProbe, TheAnswersAreFurtherApartThanTheTolerance)
{
    for (const Case &c : kCases) {
        if (c.mode != kFraction || !(c.fractionX | c.fractionY))
            continue;
        EXPECT_EQ((c.fractionX | c.fractionY) % 0x2000, 0u) << "an answer in whole codes";
        for (uint32_t y = 0; y + 1 < kSize; y++) {
            for (uint32_t x = 0; x + 1 < kSize; x++) {
                const uint32_t claim = answer(c, SWITCH_VIC_FRACTION_INTERPOLATED, x, y);
                EXPECT_GT(distance(claim, answer(c, SWITCH_VIC_FRACTION_TRUNCATED, x, y)), kTolerance);
                EXPECT_GT(distance(claim, answer(c, SWITCH_VIC_FRACTION_ROUNDED, x, y)), kTolerance);
            }
        }
    }
}

TEST(SwitchVICFractionProbe, AnInterpolatingEngineIsCleanAndNamedSo)
{
    for (const Case &c : kCases) {
        const SwitchVICFractionResult r = judge(c, engine(c, Offsets::Interpolated, Alpha::Programmed));
        EXPECT_TRUE(clean(r)) << c.fractionX << "," << c.fractionY << " mode " << c.mode << ": "
                              << r.wrong[SWITCH_VIC_FRACTION_INTERPOLATED] << " wrong, max " << r.maxError;
        EXPECT_EQ(r.edges, 0u);
        if (c.mode == kFraction && (c.fractionX | c.fractionY)) {
            EXPECT_EQ(r.wrong[SWITCH_VIC_FRACTION_TRUNCATED], (kSize - 1) * (kSize - 1));
        }
        if (c.mode == kDimension) {
            EXPECT_GT(r.wrong[SWITCH_VIC_FRACTION_TRUNCATED], (kSize - 1) * (kSize - 1) / 2);
            EXPECT_GT(r.wrong[SWITCH_VIC_FRACTION_ROUNDED], 0u);
        }
    }
}

TEST(SwitchVICFractionProbe, AnEngineThatDropsTheFractionIsNamedTruncating)
{
    const Case &c = kCases[1];  // a quarter pixel across
    const SwitchVICFractionResult r = judge(c, engine(c, Offsets::Truncated, Alpha::Programmed));
    EXPECT_FALSE(clean(r));
    EXPECT_EQ(r.wrong[SWITCH_VIC_FRACTION_TRUNCATED], 0u);
    EXPECT_EQ(r.wrong[SWITCH_VIC_FRACTION_ROUNDED], 0u);  // a quarter rounds down too
    EXPECT_EQ(r.maxError, 2u);
    EXPECT_EQ(r.firstGot, answer(c, SWITCH_VIC_FRACTION_TRUNCATED, 0, 0));
}

TEST(SwitchVICFractionProbe, AnEngineThatDropsAFractionalWidthIsNamedTruncating)
{
    for (const Case &c : kCases) {
        if (c.mode != kDimension)
            continue;
        const SwitchVICFractionResult r =
            judge(c, engine(c, Offsets::Interpolated, Alpha::Programmed, Dimensions::Truncated));
        EXPECT_FALSE(clean(r)) << c.fractionX << "," << c.fractionY;
        EXPECT_EQ(r.wrong[SWITCH_VIC_FRACTION_TRUNCATED], 0u) << c.fractionX << "," << c.fractionY;
    }
}

TEST(SwitchVICFractionProbe, AnEngineThatRoundsTheOffsetIsNamedRounding)
{
    const Case &c = kCases[6];  // three quarters down
    const SwitchVICFractionResult r = judge(c, engine(c, Offsets::Rounded, Alpha::Programmed));
    EXPECT_FALSE(clean(r));
    EXPECT_EQ(r.wrong[SWITCH_VIC_FRACTION_ROUNDED], 0u);
    EXPECT_GT(r.wrong[SWITCH_VIC_FRACTION_TRUNCATED], 0u);
}

TEST(SwitchVICFractionProbe, AnIgnoredOverrideKeepsTheSourceAlphaAndIsCaught)
{
    const Case &c = kCases[9];
    SwitchVICFractionResult r = judge(c, engine(c, Offsets::Interpolated, Alpha::Source));
    EXPECT_FALSE(clean(r));
    EXPECT_EQ(r.wrong[SWITCH_VIC_FRACTION_TRUNCATED], 0u);
    r = judge(c, engine(c, Offsets::Interpolated, Alpha::Opaque));
    EXPECT_EQ(r.wrong[SWITCH_VIC_FRACTION_ROUNDED], 0u);
    EXPECT_GT(r.wrong[SWITCH_VIC_FRACTION_INTERPOLATED], 0u);
}

TEST(SwitchVICFractionProbe, AStrayWriteIsOutsideAndControlsDecideTheProbe)
{
    std::vector<uint8_t> destination = engine(kCases[0], Offsets::Interpolated, Alpha::Programmed);
    destination[4 * kSize] = 0;  // the first row's tail
    EXPECT_EQ(judge(kCases[0], destination).outsideWritten, 1u);
    SwitchVICFractionProbeReply reply = {};
    reply.count = SWITCH_VIC_FRACTION_PROBE_CASES;
    reply.results[2].wrong[0] = 5;
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 1u);
    EXPECT_EQ(reply.cleanCases, 0x1ffbu);
    reply.results[8].status = -5;
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 0u);
}
