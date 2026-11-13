// SPDX-License-Identifier: BSD-2-Clause-Patent
// VIC 4.0's T210 format table (TegraVICFormats.h) against NVIDIA's class values (as averne's clb0b6.h gives them), the
// configurations tegra_vic_build makes for the formats beyond BGRA/NV12 (three planes, one-byte and two-byte
// elements, 4:4:4), block-linear output, and the traits that hold back what the unit has not run.
#include <gtest/gtest.h>

#include "../TegraVICConfig.h"
#include "../TegraVICRequest.h"

namespace {

constexpr uint64_t kSrcIova = 0x10000000, kDstIova = 0x18000000, kConfig = 0x80001000, kFilter = 0x80002000;
constexpr uint32_t kBgra = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
constexpr uint32_t kY420 = TEGRA_VIC_FOURCC('y', '4', '2', '0');

TegraVICSurfaceDesc surface(uint32_t fourcc, uint32_t w, uint32_t h, uint32_t lumaPitch, uint32_t chromaPitch = 0)
{
    TegraVICSurfaceDesc d = {};
    d.fourcc = fourcc;
    d.width = w;
    d.height = h;
    const TegraVICFormat *f = tegra_vic_format(fourcc);
    d.planes = f->planes > 1 ? f->planes : 0;
    uint64_t at = 0;
    for (uint32_t p = 0; p < f->planes; p++) {
        d.planeOffset[p] = at;
        d.planeBytesPerRow[p] = p && chromaPitch ? chromaPitch : lumaPitch;
        d.planeWidth[p] = tegra_vic_plane_width(f, w, p);
        d.planeHeight[p] = tegra_vic_plane_rows(f, h, p);
        at = (at + d.planeBytesPerRow[p] * d.planeHeight[p] + 0xfff) & ~0xfffull;
    }
    d.length = (tegra_vic_surface_extent(&d) + 0x3fff) & ~0x3fffull;
    return d;
}

struct Built {
    const char *why;
    TegraVICJob job;
    TegraVICImage src, dst;
};

Built build(const TegraVICSurfaceDesc &s, const TegraVICSurfaceDesc &d, const TegraVICTraits &traits = kTegraVICTraits,
            uint32_t transform = 0, bool blockLinearOut = false)
{
    Built b = {};
    b.why = tegra_vic_image_from(&s, kSrcIova, &b.src);
    if (!b.why)
        b.why = tegra_vic_image_from(&d, kDstIova, &b.dst);
    if (b.why)
        return b;
    if (blockLinearOut) {
        b.dst.blockLinear = 1;
        b.dst.blockHeightLog2 = 1;
    }
    TegraVICOp op = {};
    op.srcW = uint64_t(s.width) << 16;
    op.srcH = uint64_t(s.height) << 16;
    op.dstW = d.width;
    op.dstH = d.height;
    op.transform = transform;
    b.why = tegra_vic_build(&b.src, &b.dst, &op, &traits, kConfig, kFilter, &b.job);
    return b;
}

uint64_t outField(const TegraVICJob &j, uint32_t bit, uint32_t width)
{
    return tegra_vic_get(j.config, TEGRA_VIC_OUTPUT_SURFACE, bit, width);
}

uint64_t slotField(const TegraVICJob &j, uint32_t bit, uint32_t width)
{
    return tegra_vic_get(j.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE, bit, width);
}

#define OUT(j, name)  outField((j), name##_BIT, name##_WIDTH)
#define SLOT(j, name) slotField((j), name##_BIT, name##_WIDTH)

int64_t method(const TegraVICJob &j, uint32_t m)
{
    for (uint32_t i = 0; i < j.count; i++)
        if (j.method[i] == m)
            return j.value[i];
    return -1;
}

TEST(VicFormats, ClassValuesAreNvidias)
{
    EXPECT_EQ(TEGRA_VIC_T_L8, 1);
    EXPECT_EQ(TEGRA_VIC_T_R5G6B5, 10);
    EXPECT_EQ(TEGRA_VIC_T_A2B10G10R10, 39);
    EXPECT_EQ(TEGRA_VIC_T_A2R10G10B10, 40);
    EXPECT_EQ(TEGRA_VIC_T_R10G10B10A2, 42);
    EXPECT_EQ(TEGRA_VIC_T_Y8_U8__Y8_V8, 57);
    EXPECT_EQ(TEGRA_VIC_T_U8_Y8__V8_Y8, 59);
    EXPECT_EQ(TEGRA_VIC_T_Y8___U8V8_N444, 61);
    EXPECT_EQ(TEGRA_VIC_T_Y8___U8___V8_N444, 69);
    EXPECT_EQ(TEGRA_VIC_T_Y8___U8___V8_N420, 72);
    EXPECT_EQ(TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_V, 0x408);
    EXPECT_EQ(TEGRA_VIC_M_OUTPUT_CHROMA_V, 0x728);
}

TEST(VicFormats, EveryRowDescribesItsMemory)
{
    struct Row {
        uint32_t fourcc;
        int vic, planes, kind, bpe0, bpe1, subX, subY, packed;
    } rows[] = {
        {TEGRA_VIC_FOURCC('L', '0', '0', '8'), TEGRA_VIC_T_L8, 1, TEGRA_VIC_KIND_RGB, 1, 0, 0, 0, 0},
        {TEGRA_VIC_FOURCC('L', '5', '6', '5'), TEGRA_VIC_T_R5G6B5, 1, TEGRA_VIC_KIND_RGB, 2, 0, 0, 0, 0},
        {TEGRA_VIC_FOURCC('l', '1', '0', 'r'), TEGRA_VIC_T_A2R10G10B10, 1, TEGRA_VIC_KIND_RGB, 4, 0, 0, 0, 0},
        {TEGRA_VIC_FOURCC('4', '4', '4', 'v'), TEGRA_VIC_T_Y8___U8V8_N444, 2, TEGRA_VIC_KIND_YCC_VIDEO, 1, 2, 0, 0, 0},
        {TEGRA_VIC_FOURCC('4', '4', '4', 'f'), TEGRA_VIC_T_Y8___U8V8_N444, 2, TEGRA_VIC_KIND_YCC_FULL, 1, 2, 0, 0, 0},
        {kY420, TEGRA_VIC_T_Y8___U8___V8_N420, 3, TEGRA_VIC_KIND_YCC_VIDEO, 1, 1, 1, 1, 0},
        {TEGRA_VIC_FOURCC('f', '4', '2', '0'), TEGRA_VIC_T_Y8___U8___V8_N420, 3, TEGRA_VIC_KIND_YCC_FULL, 1, 1, 1, 1,
         0},
        {TEGRA_VIC_FOURCC('2', 'v', 'u', 'y'), TEGRA_VIC_T_U8_Y8__V8_Y8, 1, TEGRA_VIC_KIND_YCC_VIDEO, 2, 0, 1, 0, 1},
        {TEGRA_VIC_FOURCC('y', 'u', 'v', 's'), TEGRA_VIC_T_Y8_U8__Y8_V8, 1, TEGRA_VIC_KIND_YCC_VIDEO, 2, 0, 1, 0, 1},
    };

    for (const Row &r : rows) {
        const TegraVICFormat *f = tegra_vic_format(r.fourcc);
        ASSERT_NE(f, nullptr) << std::hex << r.fourcc;
        EXPECT_EQ(f->vic, r.vic);
        EXPECT_EQ(f->planes, r.planes);
        EXPECT_EQ(f->kind, r.kind);
        EXPECT_EQ(f->bpe[0], r.bpe0);
        EXPECT_EQ(f->bpe[1], r.bpe1);
        EXPECT_EQ(f->subX, r.subX);
        EXPECT_EQ(f->subY, r.subY);
        EXPECT_EQ(f->packed, r.packed);
        EXPECT_EQ(f->vicTransposed, r.packed ? 0 : r.vic) << "only packed 4:2:2 has no transposed layout";
    }
}

TEST(VicFormats, ThreePlaneSourceAndDestinationNameAllThreePlanes)
{
    const TegraVICSurfaceDesc y420 = surface(kY420, 1920, 1080, 2048, 1024);
    const TegraVICSurfaceDesc bgra = surface(kBgra, 1920, 1080, 7680);
    const Built in = build(y420, bgra);
    ASSERT_EQ(in.why, nullptr) << in.why;
    EXPECT_EQ(SLOT(in.job, SURF_FORMAT), uint64_t(TEGRA_VIC_T_Y8___U8___V8_N420));
    EXPECT_EQ(SLOT(in.job, SURF_LUMA_WIDTH), 2047u);
    EXPECT_EQ(SLOT(in.job, SURF_CHROMA_WIDTH), 1023u);
    EXPECT_EQ(SLOT(in.job, SURF_CHROMA_HEIGHT), 539u);
    EXPECT_EQ(method(in.job, TEGRA_VIC_M_SURFACE0_SLOT0_LUMA), int64_t(in.src.base[0] >> 8));
    EXPECT_EQ(method(in.job, TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U), int64_t(in.src.base[1] >> 8));
    EXPECT_EQ(method(in.job, TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_V), int64_t(in.src.base[2] >> 8));
    EXPECT_NE(in.src.base[1], in.src.base[2]);

    const Built out = build(bgra, y420);
    ASSERT_EQ(out.why, nullptr) << out.why;
    EXPECT_EQ(OUT(out.job, SURF_FORMAT), uint64_t(TEGRA_VIC_T_Y8___U8___V8_N420));
    EXPECT_EQ(method(out.job, TEGRA_VIC_M_OUTPUT_CHROMA_U), int64_t(out.dst.base[1] >> 8));
    EXPECT_EQ(method(out.job, TEGRA_VIC_M_OUTPUT_CHROMA_V), int64_t(out.dst.base[2] >> 8));
    EXPECT_EQ(method(build(bgra, bgra).job, TEGRA_VIC_M_OUTPUT_CHROMA_V), -1);

    TegraVICSurfaceDesc uneven = y420;
    uneven.planeBytesPerRow[2] = 2048;
    uneven.length = (tegra_vic_surface_extent(&uneven) + 0x3fff) & ~0x3fffull;
    EXPECT_STREQ(build(uneven, bgra).why, "chroma planes of different geometry");
}

TEST(VicFormats, OneAndTwoByteElementsAndFullResolutionChroma)
{
    const TegraVICSurfaceDesc bgra = surface(kBgra, 640, 480, 2560);
    const Built l8 = build(surface(TEGRA_VIC_FOURCC('L', '0', '0', '8'), 640, 480, 768), bgra);
    ASSERT_EQ(l8.why, nullptr) << l8.why;
    EXPECT_EQ(SLOT(l8.job, SURF_LUMA_WIDTH), 767u);
    const Built rgb565 = build(bgra, surface(TEGRA_VIC_FOURCC('L', '5', '6', '5'), 640, 480, 1280));
    ASSERT_EQ(rgb565.why, nullptr) << rgb565.why;
    EXPECT_EQ(OUT(rgb565.job, SURF_FORMAT), uint64_t(TEGRA_VIC_T_R5G6B5));
    EXPECT_EQ(OUT(rgb565.job, SURF_LUMA_WIDTH), 639u);
    const Built l10r = build(bgra, surface(TEGRA_VIC_FOURCC('l', '1', '0', 'r'), 640, 480, 2560));
    ASSERT_EQ(l10r.why, nullptr) << l10r.why;
    EXPECT_EQ(OUT(l10r.job, SURF_FORMAT), uint64_t(TEGRA_VIC_T_A2R10G10B10));
    const Built nv24 = build(surface(TEGRA_VIC_FOURCC('4', '4', '4', 'v'), 640, 480, 768, 1280), bgra);
    ASSERT_EQ(nv24.why, nullptr) << nv24.why;
    EXPECT_EQ(SLOT(nv24.job, SURF_CHROMA_WIDTH), 639u);
    EXPECT_EQ(SLOT(nv24.job, SURF_CHROMA_HEIGHT), 479u);
    EXPECT_EQ(nv24.src.rows[1], 480u);
}

TEST(VicFormats, PackedYccWaitsForItsProbeAndHasNoTransposedLayout)
{
    const TegraVICSurfaceDesc yuvs = surface(TEGRA_VIC_FOURCC('y', 'u', 'v', 's'), 640, 480, 1280);
    const TegraVICSurfaceDesc bgra = surface(kBgra, 640, 480, 2560);
    EXPECT_STREQ(build(yuvs, bgra).why, "packed 4:2:2 this engine has not proven");
    EXPECT_STREQ(build(bgra, yuvs).why, "packed 4:2:2 this engine has not proven");
    TegraVICTraits proven = kTegraVICTraits;
    proven.packedYcc = 1;
    const Built in = build(yuvs, bgra, proven);
    ASSERT_EQ(in.why, nullptr) << in.why;
    EXPECT_EQ(SLOT(in.job, SURF_LUMA_WIDTH), 639u);
    EXPECT_EQ(method(in.job, TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U), -1);
    const TegraVICSurfaceDesc turned = surface(TEGRA_VIC_FOURCC('y', 'u', 'v', 's'), 480, 640, 1024);
    EXPECT_STREQ(build(bgra, turned, proven, 4).why, "transposed output this format has no layout for");
}

TEST(VicFormats, BlockLinearOutputIsGeneric16Bx2InWholeBlocks)
{
    const TegraVICSurfaceDesc src = surface(TEGRA_VIC_FOURCC('4', '2', '0', 'v'), 1920, 1080, 2048);
    TegraVICSurfaceDesc dst = surface(kBgra, 1920, 1080, 7680);
    dst.planeHeight[0] = 1088;
    dst.length = (tegra_vic_surface_extent(&dst) + 0x3fff) & ~0x3fffull;
    const Built b = build(src, dst, kTegraVICTraits, 0, true);
    ASSERT_EQ(b.why, nullptr) << b.why;
    EXPECT_EQ(OUT(b.job, SURF_BLK_KIND), uint64_t(TEGRA_VIC_BLK_16Bx2));
    EXPECT_EQ(OUT(b.job, SURF_BLK_HEIGHT), 1u);
    EXPECT_EQ(OUT(b.job, SURF_LUMA_HEIGHT), 1087u);
    EXPECT_EQ(OUT(b.job, SURF_HEIGHT), 1079u);

    TegraVICSurfaceDesc partial = dst;
    partial.planeHeight[0] = 1080;
    EXPECT_STREQ(build(src, partial, kTegraVICTraits, 0, true).why, "block-linear plane of partial blocks");
    TegraVICSurfaceDesc turned = surface(kBgra, 1088, 1920, 4352);
    EXPECT_STREQ(build(src, turned, kTegraVICTraits, 4, true).why,
                 "transposed block-linear destination this engine has not proven");
    TegraVICTraits unproven = kTegraVICTraits;
    unproven.blockLinearOut = 0;
    EXPECT_STREQ(build(src, dst, unproven, 0, true).why, "block-linear destination this engine has not proven");
    EXPECT_EQ(OUT(build(src, dst).job, SURF_BLK_KIND), uint64_t(TEGRA_VIC_BLK_PITCH));
}

TEST(VicFormats, OnlySourcePitchesRoundTo256)
{
    for (const uint32_t pitch : {320u, 576u, 2880u}) {
        const TegraVICSurfaceDesc tight = surface(kBgra, pitch / 4, 32, pitch);
        const TegraVICSurfaceDesc wide = surface(kBgra, pitch / 4, 32, (pitch + 255) & ~255u);
        EXPECT_EQ(build(wide, tight).why, nullptr) << pitch;
        EXPECT_STREQ(build(tight, wide).why, "row pitch the engine cannot address") << pitch;
    }
}

TEST(VicFormats, PitchRuleIsATraitPerSide)
{
    const TegraVICSurfaceDesc bgra = surface(kBgra, 80, 64, 320);
    TegraVICImage im;
    ASSERT_EQ(tegra_vic_image_from(&bgra, kDstIova, &im), nullptr);
    EXPECT_STREQ(tegra_vic_check_image(&im, tegra_vic_pitch_align(&kTegraVICTraits, true)),
                 "row pitch the engine cannot address");
    EXPECT_EQ(tegra_vic_check_image(&im, tegra_vic_pitch_align(&kTegraVICTraits, false)), nullptr);
    TegraVICTraits measured = kTegraVICTraits;
    measured.sourcePitchAlign = 64;
    EXPECT_EQ(tegra_vic_check_image(&im, tegra_vic_pitch_align(&measured, true)), nullptr);
}

}  // namespace
