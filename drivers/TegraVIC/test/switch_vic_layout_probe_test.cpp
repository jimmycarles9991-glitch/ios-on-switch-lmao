// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchVICLayoutProbe.h"
extern "C" {
#include "../../SwitchVideoDecoders/AVD/BlockLinear.h"
}

namespace {
using namespace switch_vic_layout_probe;

uint32_t intended(const Case &c)
{
    return c.blockHeightLog2 == kPitch ? kPitchLaw : c.blockHeightLog2;
}

// An engine that copies the source picture into the destination's planes laid out by `law`; with `wholeBlocks` it also
// writes the rest of every block it touches, as VIC may.
std::vector<uint8_t> engine(const Case &c, uint32_t law, bool wholeBlocks = false)
{
    std::vector<uint8_t> source(kBufferBytes), destination(kBufferBytes, kPoison);
    fillSource(source.data(), c);
    const Layout s = layout(c, false), d = layout(c, true);
    for (uint32_t p = 0; p < d.planes; p++) {
        const Plane &from = s.plane[p], &to = d.plane[p];
        if (wholeBlocks)
            memset(destination.data() + to.offset, 0, uint64_t(to.pitch) * to.rows);
        for (uint32_t y = 0; y < to.height; y++) {
            for (uint32_t x = 0; x < to.rowBytes; x++) {
                const uint64_t at = to.offset + offsetUnder(law, x, y, to.pitch);
                if (at < kBufferBytes)
                    destination[at] = source[from.offset + uint64_t(y) * from.pitch + x];
            }
        }
    }
    return destination;
}

SwitchVICLayoutResult judge(const Case &c, const std::vector<uint8_t> &destination)
{
    SwitchVICLayoutResult r = {};
    compare(destination.data(), c, &r);
    return r;
}
}  // namespace

TEST(SwitchVICLayoutProbe, TheLawAtTwoGOBsIsNVDECsDeswizzle)
{
    constexpr uint32_t pitch = 192, rows = 48;
    std::vector<uint8_t> tiled(pitch * rows), linear(pitch * rows);
    for (uint32_t i = 0; i < tiled.size(); i++)
        tiled[i] = uint8_t(i * 131 + (i >> 8));
    block_linear_copy(linear.data(), pitch, tiled.data(), pitch, 0, 0, pitch, rows);
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < pitch; x++)
            ASSERT_EQ(tiled[blockLinearOffset(x, y, pitch, 1)], linear[y * pitch + x]) << x << "," << y;
    }
}

TEST(SwitchVICLayoutProbe, PositionInvertsTheLawAtEveryBlockHeight)
{
    constexpr uint32_t pitch = 192;
    for (uint32_t log2 = 0; log2 < kBlockLaws; log2++) {
        for (uint64_t offset = 0; offset < uint64_t(pitch) * (16u << log2); offset++) {
            uint32_t x = 0, y = 0;
            blockLinearPosition(offset, pitch, log2, &x, &y);
            ASSERT_LT(x, pitch);
            ASSERT_EQ(blockLinearOffset(x, y, pitch, log2), offset) << log2;
        }
    }
}

TEST(SwitchVICLayoutProbe, EveryCaseFitsItsBuffersAndBuildsWithTheShippedTraits)
{
    for (uint32_t i = 0; i < SWITCH_VIC_LAYOUT_PROBE_CASES; i++) {
        const Case &c = kCases[i];
        EXPECT_EQ(c.blockHeightLog2 == kPitch, i < kControls) << i;
        for (const bool destination : {false, true}) {
            const Layout l = layout(c, destination);
            const Plane &last = l.plane[l.planes - 1];
            EXPECT_LE(last.offset + uint64_t(last.pitch) * last.rows, kBufferBytes) << i;
            for (uint32_t p = 0; p < l.planes; p++)
                EXPECT_EQ(l.plane[p].pitch % (destination ? 64 : 256), 0u) << i;
        }
        const TegraVICImage in = image(c, false, 0x40000000), out = image(c, true, 0x40100000);
        TegraVICOp op = {};
        op.srcW = uint64_t(c.width) << 16;
        op.srcH = uint64_t(c.height) << 16;
        op.dstW = c.width;
        op.dstH = c.height;
        op.nearest = 1;
        TegraVICJob job;
        EXPECT_EQ(tegra_vic_build(&in, &out, &op, &kTegraVICTraits, 0x1000, 0x2000, &job), nullptr) << i;
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_KIND),
                  c.blockHeightLog2 == kPitch ? uint64_t(TEGRA_VIC_BLK_PITCH) : uint64_t(TEGRA_VIC_BLK_16Bx2));
    }
}

TEST(SwitchVICLayoutProbe, ARightEngineReadsBackUnderItsLawAlone)
{
    for (const Case &c : kCases) {
        const SwitchVICLayoutResult r = judge(c, engine(c, intended(c)));
        EXPECT_TRUE(clean(r)) << c.fourcc << " " << c.blockHeightLog2 << ": " << r.mismatches << " wrong";
        EXPECT_EQ(r.matchedLaws, 1u << intended(c)) << c.fourcc << " " << c.blockHeightLog2;
        EXPECT_EQ(r.padWritten, 0u);
    }
}

TEST(SwitchVICLayoutProbe, WholeBlocksWrittenArePaddingNotOutside)
{
    const Case &c = kCases[4];  // BGRA, 16 GOBs a block: 128 rows for a 40-row card
    const SwitchVICLayoutResult r = judge(c, engine(c, intended(c), true));
    EXPECT_TRUE(clean(r));
    EXPECT_GT(r.padWritten, 0u);
    EXPECT_EQ(r.outsideWritten, 0u);
}

TEST(SwitchVICLayoutProbe, APitchLinearWriteIntoABlockLinearDestinationIsNamed)
{
    const Case &c = kCases[3];
    const SwitchVICLayoutResult r = judge(c, engine(c, kPitchLaw));
    EXPECT_GT(r.mismatches, 0u);
    EXPECT_EQ(r.matchedLaws, 1u << kPitchLaw);
}

TEST(SwitchVICLayoutProbe, AnotherBlockHeightIsNamed)
{
    const Case &c = kCases[5];  // NV12 in NVDEC's layout, written with one GOB a block
    const SwitchVICLayoutResult r = judge(c, engine(c, 0));
    EXPECT_GT(r.mismatches, 0u);
    EXPECT_EQ(r.matchedLaws, 1u);
}

TEST(SwitchVICLayoutProbe, AStrayWriteIsOutside)
{
    const Case &c = kCases[2];
    std::vector<uint8_t> destination = engine(c, intended(c));
    destination[kBufferBytes - 1] = 0;
    const SwitchVICLayoutResult r = judge(c, destination);
    EXPECT_EQ(r.outsideWritten, 1u);
    EXPECT_FALSE(clean(r));
}

TEST(SwitchVICLayoutProbe, AFailedControlMarksTheProbeBroken)
{
    SwitchVICLayoutProbeReply reply = {};
    reply.count = SWITCH_VIC_LAYOUT_PROBE_CASES;
    reply.results[3].mismatches = 1;
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 1u);
    EXPECT_EQ(reply.cleanCases, 0x37u);
    reply.results[1].status = -5;
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 0u);
    EXPECT_EQ(reply.cleanCases, 0x35u);
}
