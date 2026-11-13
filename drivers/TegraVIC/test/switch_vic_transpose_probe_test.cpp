// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <functional>
#include <vector>
#include "../SwitchVICTransposeProbe.h"
#include "vic_model.h"

namespace {
using namespace switch_vic_transpose_probe;

constexpr uint64_t kSrcIova = 0x10000000, kDstIova = 0x18000000, kConfig = 0x80001000, kFilter = 0x80002000;

uint32_t field(const TegraVICJob &job, uint32_t bit, uint32_t width)
{
    return uint32_t(tegra_vic_get(job.config, TEGRA_VIC_OUTPUT_SURFACE, bit, width));
}

#define OUT_FIELD(job, name) field((job), name##_BIT, name##_WIDTH)

TegraVICJob built(const Case &c)
{
    TegraVICJob job;
    const char *why = build(c, kSrcIova, kDstIova, kConfig, kFilter, &job);
    EXPECT_EQ(why, nullptr) << why;
    return job;
}

// An engine under one reading of a transposed 4:2:2 output: N422R halves the composed picture down and takes the
// vertical siting, or N422 lays the memory out as it lies and takes the horizontal one. The format the reading does not
// name writes its chroma as a 4:4:0 plane instead. Luma and the copies are right under both.
std::vector<uint8_t> engine(const Case &c, const TegraVICJob &job, bool n422rHalvesDown)
{
    std::vector<uint8_t> out(kBufferBytes, kPoison);
    for (uint32_t v = 0; v < kSize; v++) {
        for (uint32_t u = 0; u < kSize; u++) {
            uint32_t x = 0, y = 0;
            sourceOf(c.transform, u, v, &x, &y);
            out[uint64_t(v) * kDestinationPitch + u] = luma(x, y);
        }
    }
    const auto put = [&](uint32_t k, uint32_t row, uint32_t pair) {
        uint8_t *at = out.data() + kChromaOffset + uint64_t(row) * kDestinationPitch + 2 * k;
        at[0] = uint8_t(pair);
        at[1] = uint8_t(pair >> 8);
    };
    const uint32_t format = OUT_FIELD(job, SURF_FORMAT);
    const bool transposed = (c.transform & TEGRA_VIC_ROT90) != 0;
    const bool right = !transposed || c.source == k420v ||
                       format == (n422rHalvesDown ? TEGRA_VIC_T_Y8___U8V8_N422R : TEGRA_VIC_T_Y8___U8V8_N422);
    const uint32_t halves =
        transposed ? (n422rHalvesDown ? OUT_FIELD(job, SURF_CHROMA_LOC_VERT) : OUT_FIELD(job, SURF_CHROMA_LOC_HORIZ))
                   : 0;
    for (uint32_t row = 0; row < (right ? outputChromaRows(c) : kSize / 2); row++) {
        const uint32_t v = c.source == k420v || !right ? 2 * row : row;
        for (uint32_t k = 0; k < (right ? kSize / 2 : kSize); k++)
            put(k, row, pairAt(c, k % (kSize / 2), v, c.source == k420v ? 0 : halves));
    }
    return out;
}

SwitchVICTransposeProbeReply judge(const std::function<std::vector<uint8_t>(const Case &, const TegraVICJob &)> &run)
{
    SwitchVICTransposeProbeReply reply = {};
    for (uint32_t i = 0; i < SWITCH_VIC_TRANSPOSE_PROBE_CASES; i++, reply.count++) {
        const Case &c = kCases[i];
        SwitchVICTransposeResult &r = reply.results[i];
        r.variant = c.variant;
        const std::vector<uint8_t> out = run(c, built(c));
        compare(out.data(), c, &r);
    }
    summarise(&reply);
    return reply;
}

uint32_t bits(std::initializer_list<Variant> variants)
{
    uint32_t mask = 0;
    for (const Variant v : variants)
        mask |= 1u << v;
    return mask;
}

TEST(VicTransposeProbe, EveryCaseBuildsAndPatchesOnlyWhatItNames)
{
    for (const Case &c : kCases) {
        const TegraVICJob job = built(c);
        const bool fourTwoTwo = c.source == k422v && (c.transform & TEGRA_VIC_ROT90);
        const uint32_t want = c.format >= 0       ? uint32_t(c.format)
                              : fourTwoTwo        ? TEGRA_VIC_T_Y8___U8V8_N422R
                              : c.source == k420v ? TEGRA_VIC_T_Y8___U8V8_N420
                                                  : TEGRA_VIC_T_Y8___U8V8_N422;
        EXPECT_EQ(OUT_FIELD(job, SURF_FORMAT), want) << c.variant;
        if (c.locVert >= 0)
            EXPECT_EQ(OUT_FIELD(job, SURF_CHROMA_LOC_VERT), uint32_t(c.locVert)) << c.variant;
    }
    // As built, a Center-sited turn exchanges its siting onto the composed picture's vertical axis.
    const TegraVICJob shipped = built(kCases[kShipped]);
    EXPECT_EQ(OUT_FIELD(shipped, SURF_CHROMA_LOC_HORIZ), 0u);
    EXPECT_EQ(OUT_FIELD(shipped, SURF_CHROMA_LOC_VERT), 1u);
}

TEST(VicTransposeProbe, TheShippedReadingHoldsOnAnEngineThatHalvesDown)
{
    const auto reply = judge([](const Case &c, const TegraVICJob &job) {
        return engine(c, job, true);
    });
    EXPECT_TRUE(reply.controlsClean);
    EXPECT_EQ(reply.cleanCases, bits({kCopy420Turned, kCopy422, kShipped, kShippedLeft, kSitingKept, kShippedTurn4}));
    EXPECT_EQ(reply.results[kShipped].offsetHalves, 1u);
    EXPECT_EQ(reply.results[kShippedLeft].offsetHalves, 0u);
    EXPECT_EQ(reply.results[kShippedTurn4].offsetHalves, 1u);
    EXPECT_GT(reply.results[kN422].chromaWrong + reply.results[kN422].outsideWritten, 0u);
}

TEST(VicTransposeProbe, TheOtherReadingShowsAsTheN422Cases)
{
    const auto reply = judge([](const Case &c, const TegraVICJob &job) {
        return engine(c, job, false);
    });
    EXPECT_TRUE(reply.controlsClean);
    EXPECT_EQ(reply.cleanCases, bits({kCopy420Turned, kCopy422, kN422, kN422Kept}));
    EXPECT_EQ(reply.results[kN422Kept].offsetHalves, 1u);
    EXPECT_EQ(reply.results[kN422].offsetHalves, 0u);
    EXPECT_EQ(reply.results[kShipped].lumaWrong, 0u);
    EXPECT_GT(reply.results[kShipped].chromaWrong, 0u);
}

// vic_model.h halves N422R down as the shipped reading does but samples every pair at its first column, whatever the
// siting: the probe reads that as offset 0 everywhere, so only the cases sited on the even sample come out clean.
TEST(VicTransposeProbe, OnTheModelEngine)
{
    const auto reply = judge([](const Case &c, const TegraVICJob &job) {
        std::vector<uint8_t> src(kBufferBytes), dst(kBufferBytes, kPoison);
        fillSource(src.data(), c);
        VicModel model = {};
        model.region[0] = {kSrcIova, src.data(), src.size()};
        model.region[1] = {kDstIova, dst.data(), dst.size()};
        model.regions = 2;
        model.flips = VIC_MODEL_FLIP_SURFACE;
        vic_model_run(&model, &job);
        EXPECT_EQ(model.faults, 0u) << c.variant;
        return dst;
    });
    EXPECT_TRUE(reply.controlsClean);
    EXPECT_EQ(reply.cleanCases & bits({kShippedLeft, kSitingKept}), bits({kShippedLeft, kSitingKept}));
    EXPECT_EQ(reply.results[kShipped].lumaWrong, 0u);
    EXPECT_EQ(reply.results[kShipped].offsetHalves, 0u);
    EXPECT_EQ(reply.results[kShipped].expectedHalves, 1u);
}

TEST(VicTransposeProbe, ReplyLayout)
{
    EXPECT_EQ(sizeof(SwitchVICTransposeResult), 40u);
    EXPECT_EQ(sizeof(SwitchVICTransposeProbeReply), 16u + 40u * SWITCH_VIC_TRANSPOSE_PROBE_CASES);
}

}  // namespace
