// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
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

TEST_F(SwitchNativePlaneTest, UnprovenTraitsSendCropsToTheirPitchRepresentation)
{
    constexpr SwitchDisplayTraits kUnproven = {false, false, false};
    EXPECT_FALSE(admit(2, 0, kUnproven));
    EXPECT_FALSE(admit(0, 2, kUnproven));
    layer.src.x = 2;
    layer.src.w -= 2;
    EXPECT_FALSE(admit(0, 0, kUnproven));
    layer.src.x = 0;
    layer.src.w += 2;
    layer.dst.x = -2;
    EXPECT_FALSE(admit(0, 0, kUnproven));
    layer.dst.x = 1;
    EXPECT_FALSE(admit(0, 0, kUnproven));
}

// V5a and V5c passed on boots 19 and 21: the kernel's own traits scan a decoder crop, a layer's source rectangle and a
// clipped destination in place.
TEST_F(SwitchNativePlaneTest, TheShippedTraitsScanCropsAndClipsInPlace)
{
    const SwitchDisplayTraits shipped = kSwitchDisplayTraits;
    EXPECT_TRUE(admit(2, 0, shipped));
    EXPECT_TRUE(admit(0, 2, shipped));
    layer.src.x = 2;
    layer.src.w -= 2;
    EXPECT_TRUE(admit(0, 0, shipped));
    layer.src.x = 0;
    layer.src.w += 2;
    layer.dst.x = -2;
    EXPECT_TRUE(admit(0, 0, shipped));
    layer.dst.x = 1;
    EXPECT_TRUE(admit(0, 0, shipped));
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

struct Lcg {
    uint64_t state;

    uint32_t below(uint32_t n)
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return uint32_t((state >> 33) % n);
    }

    int32_t between(int32_t low, int32_t high)
    {
        return low + int32_t(below(uint32_t(high - low + 1)));
    }
};

// A random layer of the 1280x720 picture: a source rectangle, a destination scaled within the published limits that may
// hang off any edge of the 720x1280 screen, an even decoder crop inside the padded planes.
static void randomLayer(Lcg &r, native_planes::Layer &l, uint32_t &cropX, uint32_t &cropY)
{
    cropX = 2 * r.below(129);
    cropY = 2 * r.below(9);
    const int32_t sw = r.between(64, 1280), sh = r.between(64, 720);
    l.src = {r.between(0, 1280 - sw), r.between(0, 720 - sh), sw, sh};
    const int32_t dw = std::max(16, int32_t(int64_t(sw) * r.between(30, 300) / 100));
    const int32_t dh = std::max(16, int32_t(int64_t(sh) * r.between(30, 300) / 100));
    l.dst = {r.between(-dw / 2, 720 - dw / 2), r.between(-dh / 2, 1280 - dh / 2), dw, dh};
}

// The same picture as a pitch NV12 surface of the cropped size, as its pitch representation would be: everything the
// window is told but where the source origin goes must be what imageOf programs for that surface, and the origin must be
// the crop plus imageOf's.
TEST_F(SwitchNativePlaneTest, TheOffsetsProgramWhatThePitchRepresentationWould)
{
    Lcg r{0x5eed};
    unsigned compared = 0;
    for (unsigned trial = 0; trial < 20000; ++trial) {
        uint32_t cropX = 0, cropY = 0;
        randomLayer(r, layer, cropX, cropY);
        native_planes::Layer pitch = layer;
        pitch.surface.colorMatrix = layout.color_matrix;
        native_planes::Image expected;
        const bool shown = native_planes::imageOf(pitch, 720, 1280, expected) && expected.visible;
        const bool admitted = admit(cropX, cropY, kProven);
        if (!shown) {
            EXPECT_FALSE(admitted) << trial;
            continue;
        }
        ASSERT_TRUE(admitted) << trial;
        ++compared;
        EXPECT_EQ(image.position, expected.position) << trial;
        EXPECT_EQ(image.size, expected.size) << trial;
        EXPECT_EQ(image.prescaled, expected.prescaled) << trial;
        EXPECT_EQ(image.dda, expected.dda) << trial;
        EXPECT_EQ(image.hInitial, expected.hInitial) << trial;
        EXPECT_EQ(image.vInitial, expected.vInitial) << trial;
        EXPECT_EQ(image.options, expected.options) << trial;
        EXPECT_EQ(image.hOffset, expected.hOffset + cropX) << trial;
        EXPECT_EQ(uint64_t(image.vOffset), expected.start[0] / layer.surface.stride[0] + cropY) << trial;
        EXPECT_EQ(image.colorDepth, expected.colorDepth) << trial;
    }
    EXPECT_GT(compared, 15000u);
}

// Window B's fetch through its DDAs, scaled and clipped: every output pixel samples inside the PRESCALED rectangle it
// reads from the offsets, that rectangle lies inside the cropped picture, and its luma and chroma bytes inside the bytes
// mapped for the window.
TEST_F(SwitchNativePlaneTest, AScaledClippedFetchStaysInsideThePictureAndTheMapping)
{
    Lcg r{0xf00d};
    const auto &luma = layout.planes[0], &chroma = layout.planes[1];
    unsigned fetched = 0;
    for (unsigned trial = 0; trial < 2000; ++trial) {
        uint32_t cropX = 0, cropY = 0;
        randomLayer(r, layer, cropX, cropY);
        if (!admit(cropX, cropY, kProven) || !image.visible)
            continue;
        ++fetched;
        const uint32_t xInc = image.dda & 0xffff, yInc = image.dda >> 16;
        const uint32_t width = image.size & 0xffff, height = image.size >> 16;
        const uint32_t columns = image.prescaled & 0xffff, rows = image.prescaled >> 16;
        EXPECT_LT((uint64_t(image.hInitial) + uint64_t(width - 1) * xInc) >> 12, columns) << trial;
        EXPECT_LT((uint64_t(image.vInitial) + uint64_t(height - 1) * yInc) >> 12, rows) << trial;
        EXPECT_GE(image.hOffset, cropX) << trial;
        EXPECT_GE(image.vOffset, cropY) << trial;
        EXPECT_LE(image.hOffset + columns, cropX + layer.surface.width) << trial;
        EXPECT_LE(uint32_t(image.vOffset) + rows, cropY + layer.surface.height) << trial;
        const uint32_t lumaStride = image.lineStride & 0xffff, chromaStride = image.lineStride >> 16;
        const uint8_t yLog2 = uint8_t((image.surfaceKind >> 4) & 7);
        for (uint32_t x : {image.hOffset, image.hOffset + columns - 1})
            for (uint32_t y : {uint32_t(image.vOffset), image.vOffset + rows - 1}) {
                const uint64_t lumaAt = image.start[0] + tgm_layout_offset_of(lumaStride, yLog2, x, y);
                const uint64_t chromaAt =
                    image.start[1] + tgm_layout_offset_of(chromaStride, yLog2, (x & ~1u) + 1, y / 2);
                EXPECT_GE(lumaAt, std::max(image.readLow, luma.offset)) << trial;
                EXPECT_LT(lumaAt, std::min(image.readHigh, luma.offset + luma.bytes)) << trial;
                EXPECT_GE(chromaAt, std::max(image.readLow, chroma.offset)) << trial;
                EXPECT_LT(chromaAt, std::min(image.readHigh, chroma.offset + chroma.bytes)) << trial;
            }
    }
    EXPECT_GT(fetched, 1500u);
}
}  // namespace
