// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "switch_native_surface_stubs.h"
#include "../SwitchNativePicture.h"
#include "../SwitchNativeVICImage.h"

namespace {
TegraVICPicture nativePicture()
{
    TegraVICPicture picture = {};
    picture.pitch = 1536;
    picture.rows = 736;
    picture.chromaOffset = picture.pitch * picture.rows;
    picture.bytes = uint64_t(picture.chromaOffset) * 3 / 2;
    picture.blockHeightLog2 = 1;
    picture.width = 1280;
    picture.height = 720;
    picture.sampleBytes = 1;
    return picture;
}

TegraVICSurfaceDesc nativeTarget()
{
    TegraVICSurfaceDesc target = {};
    target.fourcc = 0x34323076;
    target.width = 1280;
    target.height = 720;
    target.planes = 2;
    return target;
}
}

TEST(SwitchNativePicture, NativeLayoutKeepsPaddedPlanesCropAndPackedRange)
{
    auto picture = nativePicture();
    auto target = nativeTarget();
    picture.cropX = 16;
    picture.width = 1264;
    target.width = picture.width;
    switch_gpu_backing_layout layout = {};
    SwitchNativeSurfaceCrop crop = {};
    ASSERT_EQ(switch_native_picture(picture, target, layout, crop), nullptr);
    EXPECT_EQ(crop.x, 16u);
    EXPECT_EQ(crop.width, 1264u);
    EXPECT_EQ(layout.planes[0].width, 1536u);
    EXPECT_EQ(layout.planes[0].height, 736u);
    EXPECT_EQ(layout.color_range, 1u);
    target.fourcc = 0x34323066;
    ASSERT_EQ(switch_native_picture(picture, target, layout, crop), nullptr);
    EXPECT_EQ(layout.color_range, 2u);
    picture.sampleBytes = 2;
    picture.pitch *= 2;
    picture.chromaOffset *= 2;
    picture.bytes *= 2;
    target.fourcc = 0x78343230;
    ASSERT_EQ(switch_native_picture(picture, target, layout, crop), nullptr);
    EXPECT_EQ(layout.planes[0].bytes_per_element, 2u);
    EXPECT_EQ(layout.planes[1].bytes_per_element, 4u);
    EXPECT_EQ(layout.planes[0].width, 1536u);
    EXPECT_EQ(layout.planes[1].width, 768u);
    EXPECT_EQ(layout.color_range, 1u);
}

TEST(SwitchNativePicture, InvalidNativeGeometryCannotReplaceLayoutOrAuthority)
{
    auto picture = nativePicture();
    auto target = nativeTarget();
    switch_gpu_backing_layout layout = {};
    layout.format = 123;
    SwitchNativeSurfaceCrop crop = {1, 2, 3, 4};
    picture.chromaOffset -= 256;
    EXPECT_NE(switch_native_picture(picture, target, layout, crop), nullptr);
    EXPECT_EQ(layout.format, 123u);
    EXPECT_EQ(crop.x, 1u);
    picture.chromaOffset += 256;
    picture.cropX = UINT32_MAX - 1;
    EXPECT_NE(switch_native_picture(picture, target, layout, crop), nullptr);
    EXPECT_EQ(layout.format, 123u);
    EXPECT_EQ(crop.width, 3u);
}

TEST(SwitchNativePicture, VICReadsNativePaddedPlanesAndUsesVisibleCrop)
{
    auto picture = nativePicture();
    auto target = nativeTarget();
    picture.cropX = 16;
    picture.width = target.width = 1264;
    target.colorMatrix = SWITCH_VIC_MATRIX_709;
    SwitchNativeSurfaceView view = {};
    ASSERT_EQ(switch_native_picture(picture, target, view.layout, view.crop), nullptr);
    TegraVICImage source = {};
    ASSERT_EQ(switch_native_vic_image(view, 0x40000000, &source), nullptr);
    EXPECT_EQ(source.base[0], 0x40000000u);
    EXPECT_EQ(source.base[1], 0x40000000u + picture.chromaOffset);
    EXPECT_EQ(source.width, 1536u);
    EXPECT_EQ(source.height, 736u);
    EXPECT_EQ(source.rows[1], 368u);
    EXPECT_EQ(source.blockLinear, 1u);
    EXPECT_EQ(source.blockHeightLog2, 1u);
    EXPECT_EQ(source.standard, uint32_t(TEGRA_VIC_YCC_709));
    TegraVICOp operation = {};
    operation.srcW = uint64_t(target.width) << 16;
    operation.srcH = uint64_t(target.height) << 16;
    operation.dstW = target.width;
    operation.dstH = target.height;
    ASSERT_TRUE(switch_native_vic_crop(view, &operation));
    EXPECT_EQ(operation.srcX, uint64_t(16) << 16);
    TegraVICImage destination = {};
    destination.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    destination.width = target.width;
    destination.height = target.height;
    destination.pitch[0] = 5120;
    destination.rows[0] = target.height;
    destination.base[0] = 0x50000000;
    destination.span[0] = uint64_t(5120) * target.height;
    TegraVICJob job = {};
    EXPECT_EQ(tegra_vic_build(&source, &destination, &operation, &kTegraVICTraits, 0x80001000, 0x80002000, &job),
              nullptr);
    EXPECT_GT(job.count, 0u);
}

TEST(SwitchNativePicture, VICRefusesUnsupportedPlanesAndOutOfCropReads)
{
    SwitchNativeSurfaceView view = {};
    ASSERT_EQ(switch_native_picture(nativePicture(), nativeTarget(), view.layout, view.crop), nullptr);
    TegraVICOp operation = {};
    operation.srcX = uint64_t(view.crop.width) << 16;
    operation.srcW = 1;
    const auto original = operation;
    EXPECT_FALSE(switch_native_vic_crop(view, &operation));
    EXPECT_EQ(operation.srcX, original.srcX);
    TegraVICImage image = {};
    image.width = 123;
    view.layout.format = TEGRA_VIC_FOURCC('x', '4', '2', '0');
    EXPECT_NE(switch_native_vic_image(view, 0x40000000, &image), nullptr);
    EXPECT_EQ(image.width, 123u);
    view.layout.format = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
    view.layout.planes[1].block_height_log2 = 2;
    EXPECT_NE(switch_native_vic_image(view, 0x40000000, &image), nullptr);
    EXPECT_EQ(image.width, 123u);
}
