// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include "../TegraVICRequest.h"
#include "../SwitchVICPicturePlan.h"

namespace {
TegraVICSurfaceDesc surface(bool yuv, uint32_t width = 1920, uint32_t height = 1080)
{
    TegraVICSurfaceDesc d = {};
    d.fourcc = yuv ? TEGRA_VIC_FOURCC('4', '2', '0', 'v') : TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
    d.width = width;
    d.height = height;
    d.planes = yuv ? 2 : 0;
    d.planeBytesPerRow[0] = ((width * (yuv ? 1 : 4) + 255) & ~255u);
    d.planeHeight[0] = height;
    if (yuv) {
        d.planeBytesPerRow[1] = d.planeBytesPerRow[0];
        d.planeOffset[1] = d.planeBytesPerRow[0] * height;
        d.planeHeight[1] = height / 2;
    }
    d.length = tegra_vic_surface_extent(&d);
    return d;
}

TEST(SwitchVICColor, ExplicitMatrixOverridesResolution)
{
    for (uint32_t width : {256u, 1920u}) {
        auto d = surface(true, width, 128);
        for (const char *tag : {"ITU_R_601_4", "ITU_R_709_2", "ITU_R_2020"}) {
            d.colorMatrix = switch_vic_matrix_tag(tag);
            TegraVICImage image = {};
            ASSERT_EQ(tegra_vic_image_from(&d, 0x10000000, &image), nullptr);
            EXPECT_EQ(image.standard, d.colorMatrix - 1);
        }
    }
}

TEST(SwitchVICColor, UntaggedDefaultDoesNotInventHDMetadata)
{
    for (uint32_t width : {256u, 1920u}) {
        auto d = surface(true, width, 128);
        d.colorMatrix = switch_vic_matrix_tag(nullptr);
        TegraVICImage image = {};
        ASSERT_EQ(tegra_vic_image_from(&d, 0x10000000, &image), nullptr);
        EXPECT_EQ(image.standard, TEGRA_VIC_YCC_601);
        EXPECT_EQ(d.colorMatrix, SWITCH_VIC_MATRIX_UNTAGGED);
    }
}

TEST(SwitchVICColor, TaggedMatricesAndRangesReachPackedConfiguration)
{
    for (uint32_t matrix : {SWITCH_VIC_MATRIX_601, SWITCH_VIC_MATRIX_709, SWITCH_VIC_MATRIX_2020}) {
        for (bool full : {false, true}) {
            for (bool fromYuv : {false, true}) {
                auto yuv = surface(true), rgb = surface(false);
                yuv.colorMatrix = matrix;
                if (full)
                    yuv.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'f');
                TegraVICImage src = {}, dst = {};
                ASSERT_EQ(tegra_vic_image_from(fromYuv ? &yuv : &rgb, 0x10000000, &src), nullptr);
                ASSERT_EQ(tegra_vic_image_from(fromYuv ? &rgb : &yuv, 0x20000000, &dst), nullptr);
                TegraVICOp op = {};
                op.srcW = uint64_t(yuv.width) << 16;
                op.srcH = uint64_t(yuv.height) << 16;
                op.dstW = yuv.width;
                op.dstH = yuv.height;
                TegraVICJob job = {};
                ASSERT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
                TegraVICMatrix expected = {};
                if (fromYuv)
                    tegra_vic_matrix_ycc_to_rgb(matrix - 1, full, &kTegraVICTraits.in, &expected);
                else
                    tegra_vic_matrix_rgb_to_ycc(matrix - 1, full, &kTegraVICTraits.out, &expected);
                uint8_t packed[32] = {};
                tegra_vic_matrix_put(packed, 0, &expected);
                const auto offset = fromYuv ? TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX : TEGRA_VIC_OUT_MATRIX;
                EXPECT_EQ(memcmp(job.config + offset, packed, sizeof(packed)), 0);
            }
        }
    }
}

TEST(SwitchVICColor, Bt2020CopyConversionAndFillKeepTheExplicitMatrix)
{
    for (bool full : {false, true}) {
        auto yuv = surface(true), rgb = surface(false);
        yuv.colorMatrix = SWITCH_VIC_MATRIX_2020;
        if (full)
            yuv.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'f');
        TegraVICImage src = {}, dst = {};
        ASSERT_EQ(tegra_vic_image_from(&yuv, 0x10000000, &src), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&yuv, 0x20000000, &dst), nullptr);
        TegraVICOp op = {};
        op.srcW = uint64_t(yuv.width) << 16;
        op.srcH = uint64_t(yuv.height) << 16;
        op.dstW = yuv.width;
        op.dstH = yuv.height;
        TegraVICJob job = {};
        EXPECT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&rgb, 0x20000000, &dst), nullptr);
        EXPECT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&rgb, 0x10000000, &src), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&yuv, 0x20000000, &dst), nullptr);
        EXPECT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        op.fill = true;
        EXPECT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        yuv.colorMatrix = switch_vic_matrix_tag("SMPTE_240M_1995");
        EXPECT_NE(tegra_vic_image_from(&yuv, 0x10000000, &src), nullptr);
    }
}

TEST(SwitchVICColor, P010ByteViewPreserves2020Tag)
{
    auto d = surface(true);
    d.fourcc = TEGRA_VIC_FOURCC('x', '4', '2', '0');
    d.colorMatrix = SWITCH_VIC_MATRIX_2020;
    TegraVICPicture picture = {}, bytes = {};
    picture.sampleBytes = 2;
    picture.width = d.width;
    TegraVICSurfaceDesc desc = {};
    ASSERT_EQ(switch_vic_picture_bytes(&picture, &d, &bytes, &desc), nullptr);
    EXPECT_EQ(desc.colorMatrix, SWITCH_VIC_MATRIX_2020);
}

TEST(SwitchVICColor, FixedPointMatricesMatchIndependent6017092020ColorEquations)
{
    constexpr double weights[3][2] = {{0.299, 0.114}, {0.2126, 0.0722}, {0.2627, 0.0593}};
    const auto code = [](double value) {
        return std::clamp(std::lround(value), 0L, 255L);
    };
    for (unsigned standard = 0; standard != 3; ++standard) {
        const double kr = weights[standard][0], kb = weights[standard][1], kg = 1 - kr - kb;
        for (bool full : {false, true}) {
            TegraVICMatrix decode = {}, encode = {};
            tegra_vic_matrix_ycc_to_rgb(standard, full, &kTegraVICTraits.in, &decode);
            tegra_vic_matrix_rgb_to_ycc(standard, full, &kTegraVICTraits.out, &encode);
            for (unsigned a = 0; a <= 255; a += 17)
                for (unsigned b = 0; b <= 255; b += 17)
                    for (unsigned c = 0; c <= 255; c += 17) {
                        uint32_t input[3] = {}, output[3] = {};
                        input[0] = a * 4;
                        input[kTegraVICTraits.in.cbChannel] = b * 4;
                        input[kTegraVICTraits.in.crChannel] = c * 4;
                        tegra_vic_matrix_apply(&decode, input, output);
                        const double y = full ? a : (double(a) - 16) * 255 / 219;
                        const double cb = (double(b) - 128) * (full ? 1 : 255.0 / 224);
                        const double cr = (double(c) - 128) * (full ? 1 : 255.0 / 224);
                        const double rgb[3] = {y + 2 * (1 - kr) * cr,
                                               y - 2 * kb * (1 - kb) / kg * cb - 2 * kr * (1 - kr) / kg * cr,
                                               y + 2 * (1 - kb) * cb};
                        for (unsigned channel = 0; channel != 3; ++channel)
                            ASSERT_NEAR(output[channel] >> 2, code(rgb[channel]), 1);
                        input[0] = a * 4;
                        input[1] = b * 4;
                        input[2] = c * 4;
                        tegra_vic_matrix_apply(&encode, input, output);
                        const double luma = kr * a + kg * b + kb * c;
                        const double encodedY = full ? luma : 16 + luma * 219 / 255;
                        const double encodedCb = 128 + (c - luma) / (2 * (1 - kb)) * (full ? 1 : 224.0 / 255);
                        const double encodedCr = 128 + (a - luma) / (2 * (1 - kr)) * (full ? 1 : 224.0 / 255);
                        ASSERT_NEAR(output[0] >> 2, code(encodedY), 1);
                        ASSERT_NEAR(output[kTegraVICTraits.out.cbChannel] >> 2, code(encodedCb), 1);
                        ASSERT_NEAR(output[kTegraVICTraits.out.crChannel] >> 2, code(encodedCr), 1);
                    }
        }
    }
}
}  // namespace

TEST(SwitchVICColor, ChromaMetadataReachesBothPackedSamplingPhases)
{
    for (uint32_t site = 1; site <= 6; ++site) {
        auto d = surface(true, 256, 128);
        d.chromaTop = d.chromaBottom = site;
        TegraVICImage src = {}, dst = {};
        ASSERT_EQ(tegra_vic_image_from(&d, 0x10000000, &src), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&d, 0x20000000, &dst), nullptr);
        TegraVICOp op = {};
        op.srcW = 256ull << 16;
        op.srcH = 128ull << 16;
        op.dstW = 256;
        op.dstH = 128;
        TegraVICJob job = {};
        ASSERT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE, SURF_CHROMA_LOC_HORIZ),
                  tegra_vic_chroma_horiz(site));
        EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_VERT),
                  tegra_vic_chroma_vert(site));
    }
}

TEST(SwitchVICColor, FieldSpecificAndUnknownChromaRefuseWithoutGuessing)
{
    auto d = surface(true);
    TegraVICImage image = {};
    d.chromaTop = 1;
    d.chromaBottom = 3;
    EXPECT_NE(tegra_vic_image_from(&d, 0x10000000, &image), nullptr);
    d.chromaTop = d.chromaBottom = switch_surface_chroma_tag("DV420");
    EXPECT_NE(tegra_vic_image_from(&d, 0x10000000, &image), nullptr);
    EXPECT_EQ(switch_surface_transfer_tag("SMPTE_ST_2084_PQ"), 16u);
    EXPECT_EQ(switch_surface_primaries_tag("ITU_R_2020"), 9u);
    EXPECT_EQ(switch_surface_transfer_tag("TransferFunction#255"), 255u);
    EXPECT_EQ(switch_surface_transfer_tag("TransferFunction#256"), UINT32_MAX);
}

TEST(SwitchVICColor, TransferAndGamutChangesRequireAColorShader)
{
    auto d = surface(true, 256, 128);
    d.transfer = 16;
    d.primaries = 9;
    TegraVICImage src = {}, dst = {};
    ASSERT_EQ(tegra_vic_image_from(&d, 0x10000000, &src), nullptr);
    ASSERT_EQ(tegra_vic_image_from(&d, 0x20000000, &dst), nullptr);
    TegraVICOp op = {};
    op.srcW = 256ull << 16;
    op.srcH = 128ull << 16;
    op.dstW = 256;
    op.dstH = 128;
    TegraVICJob job = {};
    EXPECT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
    dst.transfer = 1;
    EXPECT_NE(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
    dst.transfer = 16;
    dst.primaries = 1;
    EXPECT_NE(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
}

TEST(SwitchVICColor, ExplicitSourceEncodingCannotDisappearIntoUntaggedDestination)
{
    auto d = surface(true, 256, 128);
    TegraVICImage src = {}, dst = {};
    ASSERT_EQ(tegra_vic_image_from(&d, 0x10000000, &src), nullptr);
    ASSERT_EQ(tegra_vic_image_from(&d, 0x20000000, &dst), nullptr);
    TegraVICOp op = {};
    op.srcW = 256ull << 16;
    op.srcH = 128ull << 16;
    op.dstW = 256;
    op.dstH = 128;
    TegraVICJob job = {};
    for (uint32_t transfer : {1u, 8u, 13u, 16u, 18u}) {
        src.transfer = transfer;
        EXPECT_NE(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
    }
    src.transfer = 0;
    src.primaries = 12;
    EXPECT_NE(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
}
