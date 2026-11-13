// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchVICPicturePlan.h"
#include "../SwitchVICSurfaceRange.h"

namespace {
struct Picture {
    uint64_t pages[2048] = {};
    TegraVICPicture source = {};
    TegraVICSurfaceDesc target = {};

    Picture(uint32_t sampleBytes = 1)
    {
        source.pages = pages;
        source.pageCount = 1024;
        source.pitch = 2048 * sampleBytes;
        source.rows = 1088;
        source.chromaOffset = source.pitch * source.rows;
        source.bytes = source.chromaOffset * 3 / 2;
        source.pageCount = static_cast<uint32_t>((source.bytes + 4095) / 4096);
        source.blockHeightLog2 = 1;
        source.width = 1920;
        source.height = 1080;
        source.sampleBytes = sampleBytes;
        target.fourcc = sampleBytes == 1 ? TEGRA_VIC_FOURCC('4', '2', '0', 'v') : TEGRA_VIC_FOURCC('x', '4', '2', '0');
        target.width = source.width;
        target.height = source.height;
        target.planes = 2;
        target.planeBytesPerRow[0] = target.planeBytesPerRow[1] = source.pitch;
        target.planeHeight[0] = target.height;
        target.planeHeight[1] = target.height / 2;
        target.planeWidth[0] = target.width;
        target.planeWidth[1] = target.width / 2;
        target.planeOffset[1] = source.pitch * target.height;
        target.length = target.planeOffset[1] * 3 / 2;
    }
};

TEST(SwitchVICPicture, NativeNV12IntoPersistentPitchAllocation)
{
    Picture image;
    TegraVICPicture bytes = {};
    TegraVICSurfaceDesc desc = {};
    ASSERT_EQ(switch_vic_picture_bytes(&image.source, &image.target, &bytes, &desc), nullptr);
    SwitchVICPlacement plan = {};
    ASSERT_EQ(switch_vic_picture_place(&bytes, &desc, &plan), nullptr);
    TegraVICImage src = {}, dst = {};
    TegraVICOp op = {};
    switch_vic_picture_describe(&bytes, plan, 0x30000000, 0x40000000, &src, &dst, &op);
    EXPECT_EQ(src.base[1], 0x30000000u + image.source.chromaOffset);
    EXPECT_EQ(src.rows[0], 1088u);
    EXPECT_EQ(dst.base[0], 0x40000000u);
    EXPECT_EQ(dst.base[1], 0x40000000u + image.target.planeOffset[1]);
    EXPECT_EQ(dst.pitch[0], 2048u);
    EXPECT_EQ(dst.rows[0], 1080u);
    EXPECT_EQ(dst.span[0], 2048u * 1079 + 1920u);
    EXPECT_EQ(dst.span[1], 2048u * 539 + 1920u);
    EXPECT_EQ(src.standard, dst.standard);
    EXPECT_EQ(op.dstW, 1920u);
    EXPECT_EQ(op.dstH, 1080u);
}

TEST(SwitchVICPicture, CroppedTileKeepsCanvasPlaneOffsetAndUnwrittenNeighbors)
{
    Picture image;
    image.source.cropX = 128;
    image.source.cropY = 64;
    image.source.dstX = 256;
    image.source.dstY = 100;
    image.source.width = 640;
    image.source.height = 480;
    SwitchVICPlacement plan = {};
    ASSERT_EQ(switch_vic_picture_place(&image.source, &image.target, &plan), nullptr);
    TegraVICImage src = {}, dst = {};
    TegraVICOp op = {};
    switch_vic_picture_describe(&image.source, plan, 0x30000000, 0x40000000, &src, &dst, &op);
    EXPECT_EQ(dst.base[0], 0x40000000u + 100u * 2048 + 256u);
    EXPECT_EQ(dst.base[1], 0x40000000u + image.target.planeOffset[1] + 50u * 2048 + 256u);
    EXPECT_EQ(dst.span[0], 479u * 2048 + 640u);
    EXPECT_EQ(dst.span[1], 239u * 2048 + 640u);
    EXPECT_EQ(dst.width, 640u);
    EXPECT_EQ(dst.height, 480u);
    EXPECT_EQ(op.srcX, 128u << 16);
    EXPECT_EQ(op.srcY, 64u << 16);
    EXPECT_EQ(dst.rows[1], 240u);
}

TEST(SwitchVICPicture, P010PassThroughCopiesBytesWithoutQuantizingSamples)
{
    Picture image(2);
    TegraVICPicture bytes = {};
    TegraVICSurfaceDesc desc = {};
    ASSERT_EQ(switch_vic_picture_bytes(&image.source, &image.target, &bytes, &desc), nullptr);
    EXPECT_EQ(bytes.width, 3840u);
    EXPECT_EQ(bytes.pitch, 4096u);
    EXPECT_EQ(desc.planeOffset[1], image.target.planeOffset[1]);
    EXPECT_EQ(desc.fourcc, TEGRA_VIC_FOURCC('4', '2', '0', 'v'));
    SwitchVICPlacement plan = {};
    ASSERT_EQ(switch_vic_picture_place(&bytes, &desc, &plan), nullptr);
    TegraVICImage src = {}, dst = {};
    TegraVICOp op = {};
    switch_vic_picture_describe(&bytes, plan, 0x30000000, 0x40000000, &src, &dst, &op);
    EXPECT_EQ(src.span[0], image.source.chromaOffset);
    EXPECT_EQ(op.dstW, 3840u);
    EXPECT_EQ(dst.base[1], 0x40000000u + image.target.planeOffset[1]);
    EXPECT_EQ(dst.span[1], 539u * 4096 + 3840u);
}

TEST(SwitchVICPicture, PersistentAddressesBuildValidHardwareJobs)
{
    for (uint32_t sampleBytes : {1u, 2u}) {
        for (bool cropped : {false, true}) {
            Picture image(sampleBytes);
            if (cropped) {
                image.source.cropX = 128;
                image.source.cropY = 64;
                image.source.dstX = 256;
                image.source.dstY = 100;
                image.source.width = 640;
                image.source.height = 480;
            }
            TegraVICPicture bytes = {};
            TegraVICSurfaceDesc desc = {};
            SwitchVICPlacement plan = {};
            ASSERT_EQ(switch_vic_picture_bytes(&image.source, &image.target, &bytes, &desc), nullptr);
            ASSERT_EQ(switch_vic_picture_place(&bytes, &desc, &plan), nullptr);
            TegraVICImage src = {}, dst = {};
            TegraVICOp op = {};
            switch_vic_picture_describe(&bytes, plan, 0x30000000, 0x40000000, &src, &dst, &op);
            TegraVICJob job = {};
            const char *why = tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x80001000, 0x80002000, &job);
            EXPECT_EQ(why, nullptr) << (why ? why : "") << " samples=" << sampleBytes << " cropped=" << cropped;
            EXPECT_GT(job.count, 0u);
        }
    }
}

// A 'v0a8' target: Y, CbCr at half height, then an alpha plane as big as Y.
TegraVICSurfaceDesc triPlanar(const Picture &image)
{
    TegraVICSurfaceDesc d = image.target;
    d.fourcc = TEGRA_VIC_FOURCC('v', '0', 'a', '8');
    d.planes = 3;
    d.planeOffset[2] = d.planeOffset[1] + uint64_t(d.planeBytesPerRow[1]) * d.planeHeight[1];
    d.planeBytesPerRow[2] = d.planeBytesPerRow[0];
    d.planeWidth[2] = d.planeWidth[0];
    d.planeHeight[2] = d.planeHeight[0];
    d.length = d.planeOffset[2] + uint64_t(d.planeBytesPerRow[2]) * d.planeHeight[2];
    return d;
}

TEST(SwitchVICPicture, ColourLayerFillsTheTriPlanarTargetsFirstTwoPlanes)
{
    Picture image;
    const TegraVICSurfaceDesc target = triPlanar(image);
    TegraVICPicture bytes = {};
    TegraVICSurfaceDesc desc = {};
    ASSERT_EQ(switch_vic_picture_bytes(&image.source, &target, &bytes, &desc), nullptr);
    EXPECT_EQ(desc.fourcc, TEGRA_VIC_FOURCC('4', '2', '0', 'v'));
    SwitchVICPlacement plan = {};
    ASSERT_EQ(switch_vic_picture_place(&bytes, &desc, &plan), nullptr);
    TegraVICImage src = {}, dst = {};
    TegraVICOp op = {};
    switch_vic_picture_describe(&bytes, plan, 0x30000000, 0x40000000, &src, &dst, &op);
    EXPECT_EQ(dst.base[0], 0x40000000u);
    EXPECT_EQ(dst.base[1], 0x40000000u + target.planeOffset[1]);
    EXPECT_LE(dst.base[1] + dst.span[1], 0x40000000u + target.planeOffset[2]);
    TegraVICJob job = {};
    EXPECT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x80001000, 0x80002000, &job), nullptr);
}

TEST(SwitchVICPicture, AlphaLayerLumaAloneFillsTheAlphaPlane)
{
    Picture image;
    image.source.plane = 2;
    const TegraVICSurfaceDesc target = triPlanar(image);
    TegraVICPicture bytes = {};
    TegraVICSurfaceDesc desc = {};
    ASSERT_EQ(switch_vic_picture_bytes(&image.source, &target, &bytes, &desc), nullptr);
    EXPECT_EQ(desc.fourcc, TEGRA_VIC_FOURCC('L', '0', '0', '8'));
    EXPECT_EQ(desc.planes, 1u);
    SwitchVICPlacement plan = {};
    ASSERT_EQ(switch_vic_picture_place(&bytes, &desc, &plan), nullptr);
    EXPECT_EQ(plan.band[0].start, target.planeOffset[2]);
    EXPECT_EQ(plan.pages[1], 0u);
    TegraVICImage src = {}, dst = {};
    TegraVICOp op = {};
    switch_vic_picture_describe(&bytes, plan, 0x30000000, 0x40000000, &src, &dst, &op);
    EXPECT_EQ(src.format->vic, TEGRA_VIC_T_L8);
    EXPECT_EQ(src.span[0], image.source.chromaOffset);
    EXPECT_EQ(src.base[1], 0u);
    EXPECT_EQ(dst.base[0], 0x40000000u + target.planeOffset[2]);
    EXPECT_EQ(dst.span[0], 2048u * 1079 + 1920u);
    TegraVICJob job = {};
    const char *why = tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x80001000, 0x80002000, &job);
    ASSERT_EQ(why, nullptr) << why;
    for (uint32_t i = 0; i < job.count; i++) {
        EXPECT_NE(job.method[i], uint32_t(TEGRA_VIC_M_OUTPUT_CHROMA_U));
        EXPECT_NE(job.method[i], uint32_t(TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U));
    }
}

TEST(SwitchVICPicture, PlaneSubsetsNeedAnEightBitTriPlanarTarget)
{
    Picture image;
    image.source.plane = 2;
    TegraVICPicture bytes = {};
    TegraVICSurfaceDesc desc = {};
    EXPECT_NE(switch_vic_picture_bytes(&image.source, &image.target, &bytes, &desc), nullptr);
    image.source.plane = 1;
    const TegraVICSurfaceDesc target = triPlanar(image);
    EXPECT_NE(switch_vic_picture_bytes(&image.source, &target, &bytes, &desc), nullptr);
    Picture wide(2);
    const TegraVICSurfaceDesc wideTarget = triPlanar(wide);
    EXPECT_NE(switch_vic_picture_bytes(&wide.source, &wideTarget, &bytes, &desc), nullptr);
}

TEST(SwitchVICPicture, UnsupportedWindowDoesNotChangeFutureCompatiblePictures)
{
    Picture image;
    SwitchVICPlacement plan = {};
    image.source.cropX = 1;
    EXPECT_NE(switch_vic_picture_place(&image.source, &image.target, &plan), nullptr);
    image.source.cropX = 0;
    EXPECT_EQ(switch_vic_picture_place(&image.source, &image.target, &plan), nullptr);
    image.source.pageCount = 1;
    EXPECT_NE(switch_vic_picture_place(&image.source, &image.target, &plan), nullptr);
    image.source.pageCount = static_cast<uint32_t>((image.source.bytes + 4095) / 4096);
    EXPECT_EQ(switch_vic_picture_place(&image.source, &image.target, &plan), nullptr);
}
}  // namespace

TEST(SwitchVICSurfaceRange, DescriptorBaseAndAllocationMustBothFit)
{
    EXPECT_TRUE(switch_vic_surface_extent_valid(0x9000, 0x1000, 0x8000));
    EXPECT_FALSE(switch_vic_surface_extent_valid(0x9000, 0x1000, 0x8001));
    EXPECT_FALSE(switch_vic_surface_extent_valid(0x9000, 0xa000, 0x1000));
    EXPECT_FALSE(switch_vic_surface_extent_valid(0x9000, 256, 0x8000));
    EXPECT_FALSE(switch_vic_surface_extent_valid(UINT64_MAX, UINT64_MAX - 4095, 4095));
    EXPECT_FALSE(switch_vic_surface_extent_valid(0x9000, 0, 0));
}

// Decoder pool surfaces: NV12 at 256-byte rows, whose allocation is not a whole number of pages.
TEST(SwitchVICSurfaceRange, SurfaceOwnsItsPageRoundedDescriptor)
{
    constexpr uint64_t k720p = 1280 * 720 * 3 / 2, k1080p = 1920 * 1080 * 3 / 2, k4k = 3840 * 2160 * 3 / 2;
    for (const uint64_t page : {uint64_t(4096), uint64_t(16384)}) {
        for (const uint64_t bytes : {k720p, k1080p, k4k}) {
            const uint64_t rounded = (bytes + page - 1) & ~(page - 1);
            EXPECT_TRUE(switch_vic_surface_owns_descriptor(rounded, 0, bytes, page));
            EXPECT_TRUE(switch_vic_surface_owns_descriptor(bytes, 0, bytes, page));
            EXPECT_FALSE(switch_vic_surface_owns_descriptor(rounded + page, 0, bytes, page));
            EXPECT_FALSE(switch_vic_surface_owns_descriptor(rounded, page, bytes, page));
            EXPECT_FALSE(switch_vic_surface_owns_descriptor(bytes - 1, 0, bytes, page));
        }
    }
    EXPECT_TRUE(switch_vic_surface_owns_descriptor(512 * 512 * 3 / 2, 0, 512 * 512 * 3 / 2, 16384));
    EXPECT_FALSE(switch_vic_surface_owns_descriptor(0x8000, 0, 0, 16384));
    EXPECT_FALSE(switch_vic_surface_owns_descriptor(0x8000, 0, 0x8000, 0));
}

TEST(SwitchVICSurfaceRange, SubrangesRejectOverflowAndRespectSurfaceExtent)
{
    EXPECT_TRUE(switch_vic_surface_subrange(0x8000, 0x7000, 0x1000));
    EXPECT_FALSE(switch_vic_surface_subrange(0x8000, 0x7000, 0x1001));
    EXPECT_FALSE(switch_vic_surface_subrange(0x8000, UINT64_MAX, 2));
    EXPECT_FALSE(switch_vic_surface_subrange(UINT64_MAX, UINT64_MAX - 100, 100));
    EXPECT_FALSE(switch_vic_surface_subrange(0x8000, 0x8000, 0));
}

TEST(SwitchVICPicture, PlaneExtentAndMappedAddressCannotWrap)
{
    Picture image;
    TegraVICImage view = {};
    image.target.planeOffset[1] = UINT64_MAX - 32;
    EXPECT_EQ(tegra_vic_surface_extent(&image.target), UINT64_MAX);
    EXPECT_NE(tegra_vic_image_from(&image.target, 0, &view), nullptr);
    image.target.planeBytesPerRow[1] = UINT64_MAX;
    image.target.planeHeight[1] = UINT64_MAX;
    EXPECT_EQ(tegra_vic_surface_extent(&image.target), UINT64_MAX);
    EXPECT_NE(tegra_vic_image_from(&image.target, 0, &view), nullptr);
    Picture valid;
    EXPECT_NE(tegra_vic_image_from(&valid.target, UINT64_MAX - valid.target.planeOffset[1] + 1, &view), nullptr);
}

TEST(SwitchVICPicture, ByteViewAndCropCoordinatesCannotWrap)
{
    Picture image(2);
    TegraVICPicture bytes = {};
    TegraVICSurfaceDesc desc = {};
    image.source.dstX = 0x80000000;
    EXPECT_NE(switch_vic_picture_bytes(&image.source, &image.target, &bytes, &desc), nullptr);
    Picture native;
    SwitchVICPlacement plan = {};
    native.source.cropX = UINT32_MAX - 1;
    EXPECT_NE(switch_vic_picture_place(&native.source, &native.target, &plan), nullptr);
    native.source.cropX = 0;
    native.source.cropY = UINT32_MAX - 1;
    EXPECT_NE(switch_vic_picture_place(&native.source, &native.target, &plan), nullptr);
}

TEST(SwitchVICPicture, DestinationBandsStayInsideTheirOwnPlanes)
{
    Picture image;
    SwitchVICPlacement plan = {};
    image.target.planeHeight[1] = 10;
    EXPECT_NE(switch_vic_picture_place(&image.source, &image.target, &plan), nullptr);
    image.target.planeHeight[1] = image.target.height / 2;
    image.target.planeBytesPerRow[1] = 1024;
    EXPECT_NE(switch_vic_picture_place(&image.source, &image.target, &plan), nullptr);
}
