// SPDX-License-Identifier: BSD-2-Clause-Patent
// Odd-sized subsampled surfaces through TegraVIC: assetsd's '422f' 333x74 into 332x74 (boot 19's refusal) and a '420v'
// odd both ways, described in whole chroma pairs; the rectangles and planes that stay refused; the bounce's rows; and
// what vic_model.h writes for odd copies.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../TegraVICBounce.h"
#include "../TegraVICConfig.h"
#include "vic_model.h"

namespace {

constexpr uint64_t kSrcIova = 0x10000000, kDstIova = 0x18000000, kConfig = 0x80001000, kFilter = 0x80002000;
constexpr uint64_t kBytes = 256 * 1024;
constexpr uint32_t k420v = TEGRA_VIC_FOURCC('4', '2', '0', 'v'), k422v = TEGRA_VIC_FOURCC('4', '2', '2', 'v');
constexpr uint32_t k422f = TEGRA_VIC_FOURCC('4', '2', '2', 'f'), kYuvs = TEGRA_VIC_FOURCC('y', 'u', 'v', 's');
constexpr uint8_t kPoison = 0xab;

// A surface as CoreVideo lays out an odd size: its chroma plane holds the last pair's column and row.
TegraVICImage image(uint32_t fourcc, uint32_t w, uint32_t h, uint32_t pitch, uint64_t iova)
{
    const TegraVICFormat *f = tegra_vic_format(fourcc);
    TegraVICImage im = {};
    im.format = f;
    im.width = w;
    im.height = h;
    im.standard = TEGRA_VIC_YCC_709;
    im.chromaSite = 1;
    uint64_t at = 0;
    for (uint32_t p = 0; p < f->planes; p++) {
        im.pitch[p] = pitch;
        im.rows[p] = p ? tegra_vic_plane_rows(f, tegra_vic_padded_height(f, h), p) : h;
        im.base[p] = iova + at;
        at = (at + uint64_t(pitch) * im.rows[p] + 0xfff) & ~0xfffull;
    }
    for (uint32_t p = 0; p < f->planes; p++)
        im.span[p] = kBytes - (im.base[p] - iova);
    return im;
}

TegraVICOp copy(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    TegraVICOp op = {};
    op.srcW = uint64_t(sw) << 16;
    op.srcH = uint64_t(sh) << 16;
    op.dstW = dw;
    op.dstH = dh;
    op.nearest = 1;
    return op;
}

uint64_t slotField(const TegraVICJob &j, uint32_t bit, uint32_t width)
{
    return tegra_vic_get(j.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE, bit, width);
}

uint64_t outField(const TegraVICJob &j, uint32_t bit, uint32_t width)
{
    return tegra_vic_get(j.config, TEGRA_VIC_OUTPUT_SURFACE, bit, width);
}

#define SLOT_FIELD(j, name) slotField((j), name##_BIT, name##_WIDTH)
#define OUT_FIELD(j, name)  outField((j), name##_BIT, name##_WIDTH)

const char *built(const TegraVICImage &src, const TegraVICImage &dst, const TegraVICOp &op, TegraVICJob *job)
{
    return tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, kConfig, kFilter, job);
}

TEST(VicOdd, AssetsdsOddWidth422fScalesIntoAnEvenOne)
{
    const TegraVICImage src = image(k422f, 333, 74, 768, kSrcIova), dst = image(k422f, 332, 74, 704, kDstIova);
    TegraVICJob job;
    ASSERT_STREQ(built(src, dst, copy(333, 74, 332, 74), &job), nullptr);
    EXPECT_EQ(SLOT_FIELD(job, SURF_WIDTH), 333u) << "one pixel wider: 167 whole pairs";
    EXPECT_EQ(SLOT_FIELD(job, SURF_HEIGHT), 73u);
    EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_RIGHT), uint64_t(332) << 16)
        << "the rectangle odd";
    EXPECT_EQ(OUT_FIELD(job, SURF_WIDTH), 331u);
}

TEST(VicOdd, An420vOddBothWaysIsDescribedInWholePairs)
{
    const TegraVICImage src = image(k420v, 33, 9, 256, kSrcIova), dst = image(k420v, 33, 9, 64, kDstIova);
    TegraVICJob job;
    ASSERT_EQ(built(src, dst, copy(33, 9, 33, 9), &job), nullptr);
    EXPECT_EQ(SLOT_FIELD(job, SURF_WIDTH), 33u);
    EXPECT_EQ(SLOT_FIELD(job, SURF_HEIGHT), 9u);
    EXPECT_EQ(SLOT_FIELD(job, SURF_LUMA_HEIGHT), 9u);
    EXPECT_EQ(SLOT_FIELD(job, SURF_CHROMA_HEIGHT), 4u);
    EXPECT_EQ(OUT_FIELD(job, SURF_WIDTH), 33u);
    EXPECT_EQ(OUT_FIELD(job, SURF_HEIGHT), 9u);
    EXPECT_EQ(OUT_FIELD(job, SURF_CHROMA_HEIGHT), 4u);
    EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT), 32u);
    EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM), 8u);
}

TEST(VicOdd, AnOddRectangleEndsOnlyAtTheSurfacesEdge)
{
    const TegraVICImage src = image(k420v, 33, 9, 256, kSrcIova), dst = image(k420v, 33, 9, 64, kDstIova);
    TegraVICJob job;
    TegraVICOp op = copy(31, 9, 31, 9);
    EXPECT_STREQ(built(src, dst, op, &job), "4:2:0 destination rectangle on odd coordinates");
    op.dstX = 2;
    EXPECT_EQ(built(src, dst, op, &job), nullptr);
    op = copy(33, 7, 33, 7);
    EXPECT_STREQ(built(src, dst, op, &job), "4:2:0 destination rectangle on odd coordinates");
    op.dstY = 2;
    EXPECT_EQ(built(src, dst, op, &job), nullptr);
    const TegraVICImage wide = image(k422v, 33, 8, 64, kDstIova);
    EXPECT_STREQ(built(src, wide, copy(33, 9, 31, 8), &job), "4:2:2 destination rectangle on odd columns");
    EXPECT_EQ(built(src, wide, copy(33, 9, 33, 8), &job), nullptr);
}

TEST(VicOdd, ThePlanesMustHoldThePaddedPair)
{
    TegraVICImage dst = image(k420v, 33, 9, 64, kDstIova);
    const TegraVICImage src = image(k420v, 33, 9, 256, kSrcIova);
    TegraVICJob job;
    dst.rows[1] = 4;
    EXPECT_STREQ(built(src, dst, copy(33, 9, 33, 9), &job), "plane shorter than the surface");
    const TegraVICImage tight = image(k422f, 257, 8, 256, kSrcIova);
    EXPECT_STREQ(built(tight, image(k422f, 256, 8, 512, kDstIova), copy(257, 8, 256, 8), &job),
                 "row pitch shorter than a row");
}

TEST(VicOdd, TheBounceCopiesTheLastPairWhole)
{
    EXPECT_EQ(tegra_vic_bounce_row_bytes(tegra_vic_format(k420v), 333, 0), 333u);
    EXPECT_EQ(tegra_vic_bounce_row_bytes(tegra_vic_format(k420v), 333, 1), 334u);
    EXPECT_EQ(tegra_vic_bounce_row_bytes(tegra_vic_format(k422f), 333, 1), 334u);
    EXPECT_EQ(tegra_vic_bounce_row_bytes(tegra_vic_format(kYuvs), 333, 0), 668u);
    EXPECT_EQ(tegra_vic_bounce_row_bytes(tegra_vic_format(k420v), 332, 1), 332u);
}

// A '420v' 33x9 and a '422v' 33x8 copied onto themselves on the model: every luma byte and every CbCr pair, the
// last column's and row's included, exact; nothing past the pictures written.
TEST(VicOdd, OddCopiesAreExactOnTheModel)
{
    for (const uint32_t fourcc : {k420v, k422v}) {
        const uint32_t w = 33, h = fourcc == k420v ? 9 : 8;
        std::vector<uint8_t> source(kBytes), destination(kBytes, kPoison);
        const TegraVICImage src = image(fourcc, w, h, 256, kSrcIova), dst = image(fourcc, w, h, 64, kDstIova);
        const TegraVICFormat *f = src.format;
        for (uint32_t p = 0; p < 2; p++) {
            uint8_t *plane = source.data() + (src.base[p] - kSrcIova);
            for (uint32_t y = 0; y < src.rows[p]; y++)
                for (uint32_t x = 0; x < tegra_vic_row_bytes(f, w, p); x++)
                    plane[uint64_t(y) * src.pitch[p] + x] = uint8_t(17 + 3 * x + 29 * y + 101 * p);
        }
        TegraVICJob job;
        ASSERT_EQ(built(src, dst, copy(w, h, w, h), &job), nullptr);
        VicModel model = {};
        model.region[0] = {kSrcIova, source.data(), source.size()};
        model.region[1] = {kDstIova, destination.data(), destination.size()};
        model.regions = 2;
        model.flips = VIC_MODEL_FLIP_SURFACE;
        vic_model_run(&model, &job);
        EXPECT_EQ(model.faults + model.clipped, 0u) << fourcc;
        uint32_t wrong = 0, outside = 0;
        for (uint32_t p = 0; p < 2; p++) {
            const uint64_t from = dst.base[p] - kDstIova;
            for (uint64_t at = 0; at < uint64_t(dst.pitch[p]) * dst.rows[p]; at++) {
                const uint64_t y = at / dst.pitch[p], x = at % dst.pitch[p];
                const uint8_t got = destination[from + at];
                if (x >= tegra_vic_row_bytes(f, w, p)) {
                    outside += got != kPoison;
                    continue;
                }
                wrong += got != source[src.base[p] - kSrcIova + y * src.pitch[p] + x];
            }
        }
        EXPECT_EQ(wrong, 0u) << fourcc;
        EXPECT_EQ(outside, 0u) << fourcc;
    }
}

}  // namespace
