// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchNativePlane.h"

namespace {
class SwitchNativePlaneTest : public testing::Test {
protected:
    native_planes::Layer layer;
    switch_gpu_backing_layout layout;
    native_planes::Image image;

    void SetUp() override
    {
        using namespace native_planes;
        layer = {};
        layer.kind = kKindNv12Video;
        layer.surface.format = fourcc('4', '2', '0', 'v');
        layer.surface.width = 1280;
        layer.surface.height = 720;
        layer.surface.planes = 2;
        layer.surface.stride[0] = layer.surface.stride[1] = 1280;
        layer.surface.planeHeight[0] = 720;
        layer.surface.planeHeight[1] = 360;
        layer.surface.offset[1] = 1280 * 720;
        layer.src = {0, 0, 1280, 720};
        layer.dst = {0, 437, 720, 405};
        layout = {};
        layout.format = layer.surface.format;
        layout.plane_count = 2;
        layout.color_matrix = 2;
        layout.color_range = 1;
        layout.planes[0] = {0, 1536 * 736, 1536, 736, 1536, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        layout.planes[1] = {1536 * 736, 1536 * 368, 768, 368, 1536, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
    }

    bool admit(uint32_t cropX = 0, uint32_t cropY = 0, const SwitchDisplayTraits &traits = kSwitchDisplayTraits)
    {
        return switch_native_plane_image(layer, 720, 1280, layout, cropX, cropY, image, traits);
    }

    static constexpr SwitchDisplayTraits kProven = {true, false, true};  // V5a and V5c passed
};

TEST_F(SwitchNativePlaneTest, NativePlanesUseTheirPaddedOffsetsAndPitch)
{
    ASSERT_TRUE(admit());
    EXPECT_EQ(image.start[0], 0u);
    EXPECT_EQ(image.start[1], 1536u * 736);
    EXPECT_EQ(image.lineStride, (1536u << 16) | 1536u);
    EXPECT_EQ(image.surfaceKind, 0x12u);
    EXPECT_EQ(image.readLow, 0u);
    EXPECT_EQ(image.readHigh, (1536u * (736 + 368) + 4095u) & ~4095u);
    EXPECT_EQ(image.matrix, native_planes::kBt709);
    EXPECT_EQ(image.position, 437u << 16);
    EXPECT_EQ(image.size, (405u << 16) | 720u);
}

TEST_F(SwitchNativePlaneTest, NativeFullRangeAnd2020MetadataRemainExplicit)
{
    layer.kind = native_planes::kKindNv12Full;
    layout.format = layer.surface.format = native_planes::fourcc('4', '2', '0', 'f');
    layout.color_range = 2;
    layout.color_matrix = 3;
    ASSERT_TRUE(admit());
    EXPECT_TRUE(image.fullRange);
    EXPECT_EQ(image.matrix, native_planes::kBt2020);
}

TEST_F(SwitchNativePlaneTest, CroppedOrClippedNativeImagesRequireMaterialization)
{
    EXPECT_FALSE(admit(2, 0));
    EXPECT_FALSE(admit(0, 2));
    layer.src.x = 2;
    layer.src.w -= 2;
    EXPECT_FALSE(admit());
    layer.src.x = 0;
    layer.src.w += 2;
    layer.dst.x = -2;
    EXPECT_FALSE(admit());
    layer.dst.x = 1;
    EXPECT_FALSE(admit());
}

TEST_F(SwitchNativePlaneTest, ACropNeedsBothBlockLinearOverlaysAndOffsetsProven)
{
    EXPECT_FALSE(admit(16, 8, {true, true, false}));
    EXPECT_FALSE(admit(16, 8, {false, true, true}));
    EXPECT_TRUE(admit(16, 8, kProven));
}

TEST_F(SwitchNativePlaneTest, ACropGoesInTheOffsetsWithStartOnThePlaneBases)
{
    layer.dst = {0, 0, 720, 405};
    layer.src = {100, 40, 720, 405};  // 1:1, from source pixel (116, 48) of the planes
    ASSERT_TRUE(admit(16, 8, kProven));
    EXPECT_EQ(image.start[0], 0u);
    EXPECT_EQ(image.start[1], 1536u * 736);
    EXPECT_EQ(image.hOffset, 116u);
    EXPECT_EQ(image.vOffset, 48u);
    EXPECT_EQ(image.prescaled, (406u << 16) | 721u);  // imageOf's reads: one past the last for the filter
    EXPECT_EQ(image.dda, 0x10001000u);
    EXPECT_EQ(image.options & (native_planes::kHFilterOn | native_planes::kVFilterOn), 0u);
    EXPECT_EQ(image.surfaceKind, 0x12u);
    EXPECT_EQ(image.readHigh, (1536u * (736 + 368) + 4095u) & ~4095u);  // the whole planes stay mapped
}

TEST_F(SwitchNativePlaneTest, AClippedDestinationMovesTheOriginWhereTheEdgeSamples)
{
    layer.dst = {-40, 437, 800, 450};  // 1280 -> 800 across, 40 px off the left edge
    ASSERT_TRUE(admit(0, 0, kProven));
    native_planes::Axis x;
    ASSERT_TRUE(native_planes::axisOf(0, 1280, -40, 800, 720, 1280, 8, true, x));
    EXPECT_EQ(image.hOffset, x.first);
    EXPECT_EQ(image.hInitial, x.initial);
    EXPECT_EQ(image.hOffset % 2, 0u);  // a whole 4:2:0 chroma pair
    EXPECT_EQ(image.position, 437u << 16);
    EXPECT_EQ(image.size, (450u << 16) | 720u);
    EXPECT_NE(image.options & native_planes::kHFilterOn, 0u);
}

TEST_F(SwitchNativePlaneTest, ACropPastThePlanesIsRefused)
{
    EXPECT_FALSE(admit(1536 - 1280 + 2, 0, kProven));
    EXPECT_FALSE(admit(0, 736 - 720 + 2, kProven));
}

// Window B's fetch of the cropped picture at 1:1 (START on the bases, the origin in the offsets, block-linear addresses
// as TegraGM20BLayout.h lays them out, chroma on half the lines): every pixel is the source's.
TEST_F(SwitchNativePlaneTest, TheOffsetsFetchTheCroppedPicture)
{
    layer.dst = {0, 0, 720, 405};
    layer.src = {100, 40, 720, 405};
    ASSERT_TRUE(admit(16, 8, kProven));
    const auto &luma = layout.planes[0], &chroma = layout.planes[1];
    std::vector<uint8_t> memory(chroma.offset + chroma.bytes);
    const auto lumaAt = [](uint32_t x, uint32_t y) {
        return uint8_t(x * 7 + y * 13 + (x >> 4) * (y >> 3));
    };
    const auto chromaAt = [](uint32_t x, uint32_t y) {
        return uint8_t(x * 3 + y * 29 + 7);
    };
    for (uint32_t y = 0; y < luma.height; ++y)
        for (uint32_t x = 0; x < luma.width; ++x)
            memory[luma.offset + tgm_layout_offset_of(luma.pitch, uint8_t(luma.block_height_log2), x, y)] =
                lumaAt(x, y);
    for (uint32_t y = 0; y < chroma.height; ++y)
        for (uint32_t xB = 0; xB < chroma.width * 2; ++xB)
            memory[chroma.offset + tgm_layout_offset_of(chroma.pitch, uint8_t(chroma.block_height_log2), xB, y)] =
                chromaAt(xB, y);
    const uint32_t lumaStride = image.lineStride & 0xffff, chromaStride = image.lineStride >> 16;
    const uint8_t yLog2 = uint8_t((image.surfaceKind >> 4) & 7);
    for (uint32_t line = 0; line < 405; line += 3)
        for (uint32_t pixel = 0; pixel < 720; pixel += 5) {
            const uint32_t x = image.hOffset + pixel, y = image.vOffset + line;
            ASSERT_EQ(memory[image.start[0] + tgm_layout_offset_of(lumaStride, yLog2, x, y)],
                      lumaAt(116 + pixel, 48 + line));
            const uint32_t pair = x & ~1u;
            ASSERT_EQ(memory[image.start[1] + tgm_layout_offset_of(chromaStride, yLog2, pair, y / 2)],
                      chromaAt((116 + pixel) & ~1u, (48 + line) / 2));
        }
}

TEST_F(SwitchNativePlaneTest, PlaneOverlapAndIncompleteBlocksAreRefused)
{
    layout.planes[1].offset -= 256;
    EXPECT_FALSE(admit());
    layout.planes[1].offset += 256;
    --layout.planes[1].height;
    EXPECT_FALSE(admit());
    ++layout.planes[1].height;
    --layout.planes[1].bytes;
    EXPECT_FALSE(admit());
}

TEST_F(SwitchNativePlaneTest, WrongSampleSizeRangeMatrixAndBlockHeightAreRefused)
{
    layout.planes[1].bytes_per_element = 1;
    EXPECT_FALSE(admit());
    layout.planes[1].bytes_per_element = 2;
    layout.color_range = 2;
    EXPECT_FALSE(admit());
    layout.color_range = 1;
    layout.color_matrix = 4;
    EXPECT_FALSE(admit());
    layout.color_matrix = 1;
    layout.planes[1].block_height_log2 = 2;
    EXPECT_FALSE(admit());
}

TEST_F(SwitchNativePlaneTest, OverflowCannotReduceRequiredMappedExtent)
{
    layout.planes[1].bytes = UINT64_MAX;
    EXPECT_FALSE(admit());
    layout.planes[1].bytes = 1536 * 368;
    layout.planes[1].offset = UINT64_MAX & ~255ULL;
    EXPECT_FALSE(admit());
}
}  // namespace
