// SPDX-License-Identifier: BSD-2-Clause-Patent
// Transposed VIC jobs on the layout model (vic_model.h). The model is pinned to the two hardware observations first:
// the V4 diag lines of the OLED unit under the programming that left the planes' geometry post-transpose, and
// hekate's working 270-degree job. Then V4's own probes (TegraVICProbe.h) run on it under each flip model, and the jobs
// tegra_vic_build makes with the traits they prove must land every pixel of all 8 transforms in place, BGRA and 4:2:0,
// whole surfaces, sub-rectangles and scaled.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../TegraVICConfig.h"
#include "TegraVICProbe.h"
#include "vic_model.h"

namespace {

constexpr uint64_t kSrcIova = 0x10000000, kDstIova = 0x18000000, kConfig = 0x80001000, kFilter = 0x80002000;
constexpr TegraVICChroma kMeasured = {2, 1};

struct Bench {
    std::vector<uint8_t> src, dst;
    VicModel model;
    std::vector<std::string> notes;

    explicit Bench(uint32_t flips, size_t srcBytes = 64 * 1024, size_t dstBytes = 64 * 1024)
        : src(srcBytes), dst(dstBytes)
    {
        memset(&model, 0, sizeof(model));
        model.region[0] = {kSrcIova, src.data(), src.size()};
        model.region[1] = {kDstIova, dst.data(), dst.size()};
        model.regions = 2;
        model.flips = flips;
    }

    void run(const TegraVICJob &job)
    {
        memset(dst.data(), TEGRA_VIC_POISON_BYTE, dst.size());
        vic_model_run(&model, &job);
    }

    static int Run(void *ctx, const TegraVICImage *s, const TegraVICImage *d, const TegraVICOp *op,
                   const TegraVICTraits *t, const char **why)
    {
        TegraVICJob job;
        *why = tegra_vic_build(s, d, op, t, kConfig, kFilter, &job);
        if (*why)
            return -22;
        static_cast<Bench *>(ctx)->run(job);
        return 0;
    }

    static void Note(void *ctx, const char *line)
    {
        static_cast<Bench *>(ctx)->notes.push_back(line);
    }

    TegraVICProbeEnv env()
    {
        TegraVICProbeEnv e = {};
        e.run = Run;
        e.note = Note;
        e.ctx = this;
        e.srcVa = src.data();
        e.dstVa = dst.data();
        e.srcIova = kSrcIova;
        e.srcBytes = src.size();
        e.dstIova = kDstIova;
        e.dstBytes = dst.size();
        return e;
    }

    uint32_t pixel(uint32_t pitch, uint32_t x, uint32_t y) const
    {
        uint32_t w;
        memcpy(&w, dst.data() + (size_t)y * pitch + (size_t)x * 4, 4);
        return w;
    }
};

TegraVICTraits baseTraits()
{
    TegraVICTraits t = {};
    t.sourcePitchAlign = 256;
    t.destinationPitchAlign = 64;
    t.in = t.out = kMeasured;
    return t;
}

// Traits as V4 leaves them on the model: the probes, then what the colour stages prove on the unit.
TegraVICTraits provenTraits(uint32_t flips)
{
    Bench b(flips);
    TegraVICProbeEnv e = b.env();
    TegraVICTraits t = baseTraits();
    char line[300] = "";
    EXPECT_TRUE(tegra_vic_probe_transforms(&e, &t, line, sizeof(line))) << line;
    tegra_vic_probe_placement(&e, &t);
    t.sourcePitchAlign = 64;
    t.bilinear = 1;
    t.fillThroughMatrix = 1;
    t.yccIn = TEGRA_VIC_YCC_IN_RGB | TEGRA_VIC_YCC_IN_COPY;
    return t;
}

// The unit's own lines from the boot self-test that measured kTegraVICTraits, identical on all 19 boots of 11-05 23:53
// to 11-06 23:42: the transposes and sub-rectangles it noted, in the order it ran them.
const char *const kUnitDiagLines[] = {
    "TegraVIC: diag transpose rects before, surface before the transpose (16x32 out of 32x16): transform 6; card 1,0 "
    "at "
    "byte 256, 0,1 at 4; output pixel <- card (0,0)<-0,0 (1,0)<-0,1 (0,1)<-1,0 (15,0)<-0,15 (0,31)<-31,0 "
    "(15,31)<-31,15",
    "TegraVIC: diag transpose rects before, surface after the transpose (16x32 out of 32x16): no transform; card 1,0 "
    "at "
    "byte 128, 0,1 at 4; output pixel <- card (0,0)<-0,0 (1,0)<-0,1 (0,1)<-2,0 (15,0)<-0,15 (0,31)<-- (15,31)<--",
    "TegraVIC: diag transpose rects after, surface before the transpose (16x32 out of 32x16): no transform; card 1,0 "
    "at "
    "byte 0, 0,1 at -1; output pixel <- card (0,0)<-1,0 (1,0)<-1,0 (0,1)<-3,0 (15,0)<-1,7 (0,31)<-- (15,31)<--",
    "TegraVIC: diag transpose rects after, surface after the transpose (16x32 out of 32x16): no transform; card 1,0 at "
    "byte 0, 0,1 at -1; output pixel <- card (0,0)<-1,0 (1,0)<-1,0 (0,1)<-5,0 (15,0)<-1,7 (0,31)<-- (15,31)<--",
    "TegraVIC: diag sub-rectangle transform 1 (32x16 at 8,4 in 64x48): landed elsewhere; 512 of 512 pixels off, 256 "
    "around it written, card 0,0 at 55,4 (want 39,4): whole-surface jobs only",
    "TegraVIC: diag sub-rectangle transform 2 (32x16 at 8,4 in 64x48): landed elsewhere; 512 of 512 pixels off, 512 "
    "around it written, card 0,0 at 8,43 (want 8,19): whole-surface jobs only",
    "TegraVIC: diag sub-rectangle transform 3 (32x16 at 8,4 in 64x48): landed elsewhere; 512 of 512 pixels off, 512 "
    "around it written, card 0,0 at 55,43 (want 39,19): whole-surface jobs only",
    "TegraVIC: diag sub-rectangle transform 4 (16x32 at 8,4 in 64x48): landed elsewhere; 512 of 512 pixels off, 512 "
    "around it written, card 0,0 at 55,4 (want 23,4): whole-surface jobs only",
    "TegraVIC: diag sub-rectangle transform 5 (16x32 at 8,4 in 64x48): landed elsewhere; 512 of 512 pixels off, 512 "
    "around it written, card 0,0 at 55,43 (want 23,35): whole-surface jobs only",
    "TegraVIC: diag sub-rectangle transform 7 (16x32 at 8,4 in 64x48): landed elsewhere; 512 of 512 pixels off, 128 "
    "around it written, card 0,0 at 8,43 (want 8,35): whole-surface jobs only",
};

// The model flipping around the surface is the unit: the probes on it write the unit's lines word for word, and what
// they prove is kTegraVICTraits' transform half.
TEST(TransposeModel, IsTheUnitAndGivesItsTraits)
{
    Bench b(VIC_MODEL_FLIP_SURFACE);
    TegraVICProbeEnv e = b.env();
    TegraVICTraits t = baseTraits();
    char line[300] = "";
    ASSERT_TRUE(tegra_vic_probe_transforms(&e, &t, line, sizeof(line))) << line;
    tegra_vic_probe_placement(&e, &t);
    ASSERT_EQ(b.notes.size(), std::size(kUnitDiagLines));
    for (size_t i = 0; i < b.notes.size(); i++)
        EXPECT_EQ(b.notes[i], kUnitDiagLines[i]);
    EXPECT_EQ(memcmp(t.vicFlips, kTegraVICTraits.vicFlips, sizeof(t.vicFlips)), 0);
    EXPECT_EQ(t.rotations, kTegraVICTraits.rotations);
    EXPECT_EQ(t.preTransposeRects, kTegraVICTraits.preTransposeRects);
    EXPECT_EQ(t.preTransposeSurface, kTegraVICTraits.preTransposeSurface);
    EXPECT_EQ(t.subRects, kTegraVICTraits.subRects);
    EXPECT_EQ(kTegraVICTraits.in.cbChannel, kMeasured.cbChannel);
    EXPECT_EQ(kTegraVICTraits.out.crChannel, kMeasured.crChannel);
}

// The plain transpose of the card (VIC combination 0,0,1) under convention `convention`, as V4 builds it.
TegraVICJob plainTranspose(uint32_t convention)
{
    TegraVICTraits t = baseTraits();
    t.vicFlips[0][2] = 1;
    t.rotations = 1;
    t.preTransposeRects = convention & 1;
    t.preTransposeSurface = convention >> 1;
    const TegraVICImage src = tegra_vic_probe_bgra(kSrcIova, 64 * 1024, TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H);
    const TegraVICImage dst = tegra_vic_probe_bgra(kDstIova, 64 * 1024, TEGRA_VIC_CARD_H, TEGRA_VIC_CARD_W);
    const TegraVICOp op = tegra_vic_probe_op(TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H, TEGRA_VIC_CARD_H, TEGRA_VIC_CARD_W, 0);
    TegraVICJob job;
    const char *why = tegra_vic_build(&src, &dst, &op, &t, kConfig, kFilter, &job);
    EXPECT_EQ(why, nullptr) << why;
    return job;
}

std::string points(const Bench &b)
{
    char text[200];
    tegra_vic_probe_points(b.dst.data(), TEGRA_VIC_CARD_H, TEGRA_VIC_CARD_W, text, sizeof(text));
    return text;
}

uint64_t field(const TegraVICJob &j, uint32_t base, uint32_t bit, uint32_t width)
{
    return tegra_vic_get(j.config, base, bit, width);
}

#define FIELD(job, base, name) field((job), (base), name##_BIT, name##_WIDTH)

// The device's lines (V4 diag, transforms 0xf): rectangles and surface after the transpose, and rectangles and surface
// size before it with the luma geometry still post-transpose (LumaWidth 63 from the 256-byte pitch, LumaHeight 31).
TEST(TransposeModel, ReproducesTheDeviceDiagLines)
{
    Bench b(VIC_MODEL_FLIP_RECT);
    TegraVICProbeEnv e = b.env();
    tegra_vic_probe_draw_card(&e);

    b.run(plainTranspose(0));
    EXPECT_EQ(points(b), " (0,0)<-1,0 (1,0)<-1,0 (0,1)<-5,0 (15,0)<-1,7 (0,31)<-- (15,31)<--");

    TegraVICJob job = plainTranspose(3);
    EXPECT_EQ(FIELD(job, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH), 31u);
    EXPECT_EQ(FIELD(job, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT), 63u);
    TEGRA_VIC_SET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH, 63);
    TEGRA_VIC_SET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT, 31);
    b.run(job);
    EXPECT_EQ(points(b), " (0,0)<-0,0 (1,0)<-0,1 (0,1)<-2,0 (15,0)<-0,15 (0,31)<-- (15,31)<--");
    EXPECT_EQ(tegra_vic_probe_find(b.dst.data(), b.dst.size(), tegra_vic_card_pixel(1, 0)), 128);
    EXPECT_EQ(b.model.faults, 0u);
}

// hekate's Nyx rotation, programmed as it does: a 1280x720 frame, OutputFlipX and OutputTranspose, the target, the
// output surface and its luma geometry all the source's (1279 x 719), into a 720-pixel-stride framebuffer. The model
// must write each of the 1280 rows of 720 pixels once, a quarter turn of the frame (IOSurfaceAccelerator's 7).
TEST(TransposeModel, HekateNyxRotation)
{
    constexpr uint32_t W = 1280, H = 720;
    Bench b(VIC_MODEL_FLIP_RECT, (size_t)W * H * 4, (size_t)H * W * 4);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            const uint32_t w = 0xff000000u | (x * 2654435761u + y * 40503u) >> 8;
            memcpy(b.src.data() + ((size_t)y * W + x) * 4, &w, 4);
        }
    TegraVICJob job = {};
    uint8_t *c = job.config;
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_X, 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE, 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT, W - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM, H - 1);
    const uint32_t os = TEGRA_VIC_OUTPUT_SURFACE, s = TEGRA_VIC_SLOTS, sf = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE;
    for (uint32_t base : {os, sf}) {
        TEGRA_VIC_SET(c, base, SURF_FORMAT, TEGRA_VIC_T_A8R8G8B8);
        TEGRA_VIC_SET(c, base, SURF_WIDTH, W - 1);
        TEGRA_VIC_SET(c, base, SURF_HEIGHT, H - 1);
        TEGRA_VIC_SET(c, base, SURF_LUMA_WIDTH, W - 1);
        TEGRA_VIC_SET(c, base, SURF_LUMA_HEIGHT, H - 1);
    }
    TEGRA_VIC_SET(c, s, SLOT_ENABLE, 1);
    TEGRA_VIC_SET(c, s, SLOT_SOURCE_RIGHT, (uint64_t)(W - 1) << 16);
    TEGRA_VIC_SET(c, s, SLOT_SOURCE_BOTTOM, (uint64_t)(H - 1) << 16);
    TEGRA_VIC_SET(c, s, SLOT_DEST_RIGHT, W - 1);
    TEGRA_VIC_SET(c, s, SLOT_DEST_BOTTOM, H - 1);
    job.method[0] = TEGRA_VIC_M_OUTPUT_LUMA;
    job.value[0] = (uint32_t)(kDstIova >> 8);
    job.method[1] = TEGRA_VIC_M_SURFACE0_SLOT0_LUMA;
    job.value[1] = (uint32_t)(kSrcIova >> 8);
    job.count = 2;
    b.run(job);
    EXPECT_EQ(b.model.writes, (uint64_t)W * H);
    EXPECT_EQ(b.model.clipped + b.model.faults, 0u);
    uint64_t off = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t u, v, want;
            tegra_vic_transform_point(TEGRA_VIC_ROT90 | TEGRA_VIC_FLIP_H | TEGRA_VIC_FLIP_V, W, H, x, y, &u, &v);
            memcpy(&want, b.src.data() + ((size_t)y * W + x) * 4, 4);
            off += b.pixel(H * 4, u, v) != want;
        }
    EXPECT_EQ(off, 0u);
}

class TransposeProbe : public testing::TestWithParam<uint32_t> {};

// V4's search on the model: all 8 transforms, the plain transpose's four lines (the order V4 tries them), everything
// before the transpose chosen.
TEST_P(TransposeProbe, FindsAllEight)
{
    Bench b(GetParam());
    TegraVICProbeEnv e = b.env();
    TegraVICTraits t = baseTraits();
    char line[300] = "";
    ASSERT_TRUE(tegra_vic_probe_transforms(&e, &t, line, sizeof(line))) << line;
    EXPECT_EQ(t.rotations, 0xffu);
    EXPECT_EQ(t.preTransposeRects, 1u);
    EXPECT_EQ(t.preTransposeSurface, 1u);
    ASSERT_EQ(b.notes.size(), 4u);
    EXPECT_NE(b.notes[0].find("rects before, surface before the transpose (16x32 out of 32x16): transform 6; card 1,0 "
                              "at byte 256, 0,1 at 4;"),
              std::string::npos)
        << b.notes[0];
    for (size_t i = 1; i < 4; i++)
        EXPECT_NE(b.notes[i].find(": no transform;"), std::string::npos) << b.notes[i];
    EXPECT_NE(b.notes[3].find("rects after, surface after the transpose"), std::string::npos) << b.notes[3];
    EXPECT_NE(b.notes[3].find(" (0,0)<-1,0 (1,0)<-1,0 (0,1)<-5,0 (15,0)<-1,7 (0,31)<-- (15,31)<--"), std::string::npos)
        << b.notes[3];
    for (uint32_t tr = 0; tr < 8; tr++)
        EXPECT_EQ(t.vicFlips[tr][2], (tr & TEGRA_VIC_ROT90) ? 1 : 0) << "transform " << tr;
}

// Sub-rectangles: proven for every transform where the flips mirror within the rectangle; where they mirror around the
// surface, only the transforms without a flip, the others noted and their sub-rectangle jobs refused.
TEST_P(TransposeProbe, Placement)
{
    Bench b(GetParam());
    TegraVICProbeEnv e = b.env();
    TegraVICTraits t = baseTraits();
    char line[300] = "";
    ASSERT_TRUE(tegra_vic_probe_transforms(&e, &t, line, sizeof(line))) << line;
    b.notes.clear();
    tegra_vic_probe_placement(&e, &t);
    if (GetParam() != VIC_MODEL_FLIP_SURFACE) {
        EXPECT_EQ(t.subRects, 0xffu);
        EXPECT_TRUE(b.notes.empty()) << b.notes[0];
        return;
    }
    uint32_t unflipped = 0;
    for (uint32_t tr = 0; tr < 8; tr++)
        if (!t.vicFlips[tr][0] && !t.vicFlips[tr][1])
            unflipped |= 1u << tr;
    EXPECT_EQ(t.subRects, unflipped);
    EXPECT_EQ(b.notes.size(), (size_t)(8 - __builtin_popcount(unflipped)));
    ASSERT_FALSE(b.notes.empty());
    EXPECT_NE(b.notes[0].find("landed elsewhere"), std::string::npos) << b.notes[0];
    const uint32_t refused = __builtin_ctz(~unflipped & 0xff);
    const TegraVICImage src = tegra_vic_probe_bgra(kSrcIova, 64 * 1024, TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H);
    const TegraVICImage dst = tegra_vic_probe_bgra(kDstIova, 64 * 1024, TEGRA_VIC_PLACE_W, TEGRA_VIC_PLACE_H);
    const bool rot = refused & TEGRA_VIC_ROT90;
    TegraVICOp op = tegra_vic_probe_op(TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H, rot ? TEGRA_VIC_CARD_H : TEGRA_VIC_CARD_W,
                                       rot ? TEGRA_VIC_CARD_W : TEGRA_VIC_CARD_H, refused);
    op.dstX = 2;
    TegraVICJob job;
    const char *why = tegra_vic_build(&src, &dst, &op, &t, kConfig, kFilter, &job);
    EXPECT_STREQ(why, "destination sub-rectangle this transform has not proven");
}

INSTANTIATE_TEST_SUITE_P(FlipModels, TransposeProbe,
                         testing::Values(VIC_MODEL_FLIP_RECT, VIC_MODEL_FLIP_SURFACE, VIC_MODEL_FLIP_AFTER));

uint32_t hashPixel(uint32_t x, uint32_t y)
{
    return 0xff000000u | ((x * 73856093u) ^ (y * 19349663u)) >> 8;
}

class TransposeJobs : public testing::TestWithParam<uint32_t> {};

// Every transform, whole surface and a sub-rectangle, an odd-sized card into rows longer than the picture.
TEST_P(TransposeJobs, EveryTransformLandsInPlace)
{
    const TegraVICTraits t = provenTraits(GetParam());
    constexpr uint32_t SW = 48, SH = 20, DP = 384;
    for (uint32_t tr = 0; tr < 8; tr++)
        for (int sub = 0; sub < 2; sub++) {
            Bench b(GetParam());
            for (uint32_t y = 0; y < SH; y++)
                for (uint32_t x = 0; x < SW; x++) {
                    const uint32_t w = hashPixel(x, y);
                    memcpy(b.src.data() + (size_t)y * 256 + x * 4, &w, 4);
                }
            const bool rot = tr & TEGRA_VIC_ROT90;
            const uint32_t w = rot ? SH : SW, h = rot ? SW : SH, ox = sub ? 10 : 0, oy = sub ? 6 : 0;
            TegraVICImage src = tegra_vic_probe_bgra(kSrcIova, b.src.size(), SW, SH);
            TegraVICImage dst = tegra_vic_probe_bgra(kDstIova, b.dst.size(), w + 2 * ox, h + 2 * oy);
            dst.pitch[0] = DP;
            TegraVICOp op = tegra_vic_probe_op(SW, SH, w, h, tr);
            op.dstX = ox;
            op.dstY = oy;
            TegraVICJob job;
            const char *why = tegra_vic_build(&src, &dst, &op, &t, kConfig, kFilter, &job);
            if (sub && !(t.subRects & (1u << tr))) {
                EXPECT_STREQ(why, "destination sub-rectangle this transform has not proven") << "transform " << tr;
                continue;
            }
            ASSERT_EQ(why, nullptr) << why;
            b.run(job);
            EXPECT_EQ(b.model.writes, (uint64_t)SW * SH) << "transform " << tr;
            EXPECT_EQ(b.model.clipped + b.model.faults, 0u) << "transform " << tr;
            uint32_t off = 0, around = 0;
            for (uint32_t y = 0; y < SH; y++)
                for (uint32_t x = 0; x < SW; x++) {
                    uint32_t u, v;
                    tegra_vic_transform_point(tr, SW, SH, x, y, &u, &v);
                    off += b.pixel(DP, ox + u, oy + v) != hashPixel(x, y);
                }
            for (uint32_t y = 0; y < dst.height; y++)
                for (uint32_t x = 0; x < dst.width; x++)
                    around +=
                        (x < ox || x >= ox + w || y < oy || y >= oy + h) && b.pixel(DP, x, y) != TEGRA_VIC_POISON_PIXEL;
            EXPECT_EQ(off, 0u) << "transform " << tr << (sub ? " sub-rectangle" : "");
            EXPECT_EQ(around, 0u) << "transform " << tr << (sub ? " sub-rectangle" : "");
        }
}

// Scaled quarter turns: a 64x32 card turned into 96x64, the output's width from the source's height (3x), its height
// from the source's width (1x).
TEST_P(TransposeJobs, ScaledQuarterTurns)
{
    const TegraVICTraits t = provenTraits(GetParam());
    for (uint32_t tr : {4u, 5u, 6u, 7u}) {
        Bench b(GetParam());
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 64; x++) {
                const uint32_t w = hashPixel(x, y);
                memcpy(b.src.data() + (size_t)y * 256 + x * 4, &w, 4);
            }
        TegraVICImage src = tegra_vic_probe_bgra(kSrcIova, b.src.size(), 64, 32);
        TegraVICImage dst = tegra_vic_probe_bgra(kDstIova, b.dst.size(), 96, 64);
        dst.pitch[0] = 384;
        TegraVICOp op = tegra_vic_probe_op(64, 32, 96, 64, tr);
        TegraVICJob job;
        const char *why = tegra_vic_build(&src, &dst, &op, &t, kConfig, kFilter, &job);
        ASSERT_EQ(why, nullptr) << why;
        b.run(job);
        EXPECT_EQ(b.model.clipped + b.model.faults, 0u);
        uint32_t off = 0;
        for (uint32_t y = 0; y < 96; y++)
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t u, v;
                tegra_vic_transform_point(tr, 64, 96, x, y, &u, &v);
                off += b.pixel(384, u, v) != hashPixel(x, y / 3);
            }
        EXPECT_EQ(off, 0u) << "transform " << tr;
    }
}

// Screen Recording's shape and a portrait video's: the 720x1280 BGRA LCD (2880-byte rows) a quarter turn either way
// into a 1280x720 '420f', in 16x16 flat patches. Every luma sample and every CbCr pair lands where the turned picture
// has its colour, and nothing outside the two planes is touched.
TEST_P(TransposeJobs, QuarterTurnsInto420f)
{
    const TegraVICTraits t = provenTraits(GetParam());
    constexpr uint32_t SW = 720, SH = 1280, DW = 1280, DH = 720, SP = 2880, DP = 1280;
    const auto colour = [](uint32_t x, uint32_t y) {
        const uint32_t p = (x / 16) * 37 + (y / 16) * 101;
        return 0xff000000u | (p * 97 & 0xff) << 16 | (p * 53 & 0xff) << 8 | (p * 29 & 0xff);
    };
    TegraVICMatrix m;
    tegra_vic_matrix_rgb_to_ycc(TEGRA_VIC_YCC_709, 1, &kMeasured, &m);
    const auto ycc = [&](uint32_t argb, uint32_t out[3]) {
        const uint32_t in10[3] = {((argb >> 16) & 0xff) << 2, ((argb >> 8) & 0xff) << 2, (argb & 0xff) << 2};
        uint32_t o[3];
        tegra_vic_matrix_apply(&m, in10, o);
        out[0] = o[0] >> 2;
        out[1] = o[kMeasured.cbChannel] >> 2;
        out[2] = o[kMeasured.crChannel] >> 2;
    };
    for (uint32_t tr : {4u, 7u}) {
        const size_t lumaBytes = (size_t)DP * DH, planes = lumaBytes + lumaBytes / 2;
        Bench b(GetParam(), (size_t)SP * SH, planes + 4096);
        for (uint32_t y = 0; y < SH; y++)
            for (uint32_t x = 0; x < SW; x++) {
                const uint32_t w = colour(x, y);
                memcpy(b.src.data() + (size_t)y * SP + x * 4, &w, 4);
            }
        TegraVICImage src = tegra_vic_probe_bgra(kSrcIova, b.src.size(), SW, SH);
        src.pitch[0] = SP;
        TegraVICImage dst = {};
        dst.format = tegra_vic_format(TEGRA_VIC_FOURCC('4', '2', '0', 'f'));
        dst.width = DW;
        dst.height = DH;
        dst.pitch[0] = dst.pitch[1] = DP;
        dst.rows[0] = DH;
        dst.rows[1] = DH / 2;
        dst.base[0] = kDstIova;
        dst.span[0] = lumaBytes;
        dst.base[1] = kDstIova + lumaBytes;
        dst.span[1] = lumaBytes / 2;
        dst.standard = TEGRA_VIC_YCC_709;
        TegraVICOp op = tegra_vic_probe_op(SW, SH, DW, DH, tr);
        op.nearest = 0;
        TegraVICJob job;
        const char *why = tegra_vic_build(&src, &dst, &op, &t, kConfig, kFilter, &job);
        ASSERT_EQ(why, nullptr) << why;
        b.run(job);
        EXPECT_EQ(b.model.writes, (uint64_t)DW * DH + (uint64_t)DW * DH / 4) << "transform " << tr;
        EXPECT_EQ(b.model.clipped + b.model.faults, 0u) << "transform " << tr;
        std::vector<uint32_t> want((size_t)DW * DH);
        for (uint32_t y = 0; y < SH; y++)
            for (uint32_t x = 0; x < SW; x++) {
                uint32_t u, v;
                tegra_vic_transform_point(tr, SW, SH, x, y, &u, &v);
                want[(size_t)v * DW + u] = colour(x, y);
            }
        uint32_t lumaOff = 0, chromaOff = 0;
        for (uint32_t v = 0; v < DH; v++)
            for (uint32_t u = 0; u < DW; u++) {
                uint32_t e[3];
                ycc(want[(size_t)v * DW + u], e);
                lumaOff += b.dst[(size_t)v * DP + u] != e[0];
                if ((u | v) & 1)
                    continue;
                const uint8_t *cq = b.dst.data() + lumaBytes + (size_t)(v / 2) * DP + (u / 2) * 2;
                chromaOff += cq[0] != e[1] || cq[1] != e[2];
            }
        EXPECT_EQ(lumaOff, 0u) << "transform " << tr;
        EXPECT_EQ(chromaOff, 0u) << "transform " << tr;
        for (size_t i = planes; i < b.dst.size(); i++)
            ASSERT_EQ(b.dst[i], TEGRA_VIC_POISON_BYTE) << "past the planes";
    }
}

INSTANTIATE_TEST_SUITE_P(FlipModels, TransposeJobs,
                         testing::Values(VIC_MODEL_FLIP_RECT, VIC_MODEL_FLIP_SURFACE, VIC_MODEL_FLIP_AFTER));

}  // namespace
