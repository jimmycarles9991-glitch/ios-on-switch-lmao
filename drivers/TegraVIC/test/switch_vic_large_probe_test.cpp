// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchVICLargeProbe.h"
#include "vic_model.h"

namespace {
using namespace switch_vic_large_probe;

constexpr uint64_t kSrcIova = 0x10000000, kDstIova = 0x20000000, kConfig = 0x80001000, kFilter = 0x80002000;

uint32_t toBgra(uint32_t y, uint32_t cb, uint32_t cr)
{
    const TegraVICTraits &t = kTegraVICTraits;
    TegraVICMatrix m;
    tegra_vic_matrix_ycc_to_rgb(TEGRA_VIC_YCC_601, false, &t.in, &m);
    uint32_t in10[3] = {}, out10[3] = {};
    in10[0] = y << 2 | y >> 6;
    in10[t.in.cbChannel] = cb << 2 | cb >> 6;
    in10[t.in.crChannel] = cr << 2 | cr >> 6;
    tegra_vic_matrix_apply(&m, in10, out10);
    return 0xff000000u | (out10[0] >> 2) << 16 | (out10[1] >> 2) << 8 | (out10[2] >> 2);
}

// An engine that scales by sampling each output pixel's centre and turns as IOSurfaceAccelerator's transforms do,
// reading the source in its layout. `asPitch` reads a block-linear source as if it were pitch linear; `unturned` ignores
// the quarter turn.
std::vector<uint8_t> engine(const Case &c, bool asPitch = false, bool unturned = false)
{
    std::vector<uint8_t> source(kBufferBytes), out(kBufferBytes, kPoison);
    fillSource(source.data(), c);
    Case read = c;
    if (asPitch)
        read.sourceLog2 = kPitch;
    if (unturned)
        read.transform = 0;
    const Layout s = sourceLayout(c), d = destinationLayout(c);
    const bool turned = (c.transform & TEGRA_VIC_ROT90) != 0;
    const uint32_t tw = turned ? c.sourceHeight : c.sourceWidth, th = turned ? c.sourceWidth : c.sourceHeight;
    for (uint32_t v = 0; v < c.destinationHeight; v++) {
        for (uint32_t u = 0; u < c.destinationWidth; u++) {
            const uint32_t cx = uint32_t((2 * uint64_t(u) + 1) * tw / (2 * c.destinationWidth));
            const uint32_t cy = uint32_t((2 * uint64_t(v) + 1) * th / (2 * c.destinationHeight));
            uint32_t x = 0, y = 0;
            sourceOf(read, cx, cy, &x, &y);
            x = x < c.sourceWidth ? x : c.sourceWidth - 1;
            y = y < c.sourceHeight ? y : c.sourceHeight - 1;
            const uint8_t luma = source[sourceOffset(read, s.plane[0], x, y)];
            const uint64_t chroma = sourceOffset(read, s.plane[1], x & ~1u, y / 2);
            const uint8_t cb = source[chroma], cr = source[chroma + 1];
            if (c.destination == kBGRA) {
                const uint32_t pixel = toBgra(luma, cb, cr);
                memcpy(out.data() + d.plane[0].offset + uint64_t(v) * d.plane[0].pitch + 4 * u, &pixel, 4);
                continue;
            }
            out[d.plane[0].offset + uint64_t(v) * d.plane[0].pitch + u] = luma;
            if (!(u & 1) && !(v & 1)) {
                uint8_t *pair = out.data() + d.plane[1].offset + uint64_t(v / 2) * d.plane[1].pitch + u;
                pair[0] = cb;
                pair[1] = cr;
            }
        }
    }
    return out;
}

SwitchVICLargeResult judge(const Case &c, const std::vector<uint8_t> &out)
{
    SwitchVICLargeResult r = {};
    compare(out.data(), c, &r);
    return r;
}

TEST(VicLargeProbe, EveryCaseBuildsAsOneJob)
{
    for (const Case &c : kCases) {
        TegraVICJob job;
        const char *why = build(c, kSrcIova, kDstIova, kConfig, kFilter, &job);
        EXPECT_EQ(why, nullptr) << c.sourceWidth << "x" << c.sourceHeight << ": " << why;
        const Layout s = sourceLayout(c);
        EXPECT_LE(s.plane[1].offset + uint64_t(s.plane[1].pitch) * s.plane[1].rows, kBufferBytes);
        const Layout d = destinationLayout(c);
        EXPECT_LE(d.plane[d.planes - 1].offset + uint64_t(d.plane[d.planes - 1].pitch) * d.plane[d.planes - 1].rows,
                  kBufferBytes);
    }
}

TEST(VicLargeProbe, AnEngineThatScalesAndTurnsIsCleanWithMostPixelsChecked)
{
    for (uint32_t i = 0; i < SWITCH_VIC_LARGE_PROBE_CASES; i++) {
        const Case &c = kCases[i];
        const SwitchVICLargeResult r = judge(c, engine(c));
        EXPECT_TRUE(clean(r)) << i << ": wrong " << r.wrong << " unwritten " << r.unwritten << " outside "
                              << r.outsideWritten << " at " << r.firstX << "," << r.firstY;
        EXPECT_GT(r.checked, c.destinationWidth * c.destinationHeight / 4) << i;
        EXPECT_LE(r.maxError, kTolerance) << i;
    }
}

TEST(VicLargeProbe, ALayoutOrTurnMistakeIsCaught)
{
    const Case &turn = kCases[1];
    EXPECT_GT(judge(turn, engine(turn, true)).wrong, 0u);
    EXPECT_GT(judge(turn, engine(turn, false, true)).wrong + judge(turn, engine(turn, false, true)).unwritten, 0u);
    const Case &wide = kCases[3];
    EXPECT_GT(judge(wide, engine(wide, true)).wrong, 0u);
}

// The pitch-linear cases on vic_model.h, which scales by nearest sampling and writes 4:2:0 chroma from each 2x2's top
// left pixel, as the unit measured.
TEST(VicLargeProbe, PitchCasesOnTheModelEngine)
{
    for (const uint32_t i : {0u, 4u}) {
        const Case &c = kCases[i];
        std::vector<uint8_t> src(kBufferBytes), dst(kBufferBytes, kPoison);
        fillSource(src.data(), c);
        TegraVICJob job;
        ASSERT_EQ(build(c, kSrcIova, kDstIova, kConfig, kFilter, &job), nullptr);
        VicModel model = {};
        model.region[0] = {kSrcIova, src.data(), src.size()};
        model.region[1] = {kDstIova, dst.data(), dst.size()};
        model.regions = 2;
        model.flips = VIC_MODEL_FLIP_SURFACE;
        vic_model_run(&model, &job);
        EXPECT_EQ(model.faults, 0u) << i;
        const SwitchVICLargeResult r = judge(c, dst);
        EXPECT_TRUE(clean(r)) << i << ": wrong " << r.wrong << " unwritten " << r.unwritten << " outside "
                              << r.outsideWritten;
    }
}

TEST(VicLargeProbe, ReplyLayout)
{
    EXPECT_EQ(sizeof(SwitchVICLargeResult), 48u);
    EXPECT_EQ(sizeof(SwitchVICLargeProbeReply), 16u + 48u * SWITCH_VIC_LARGE_PROBE_CASES);
}

}  // namespace
