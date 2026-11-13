// drivers/NativeMobileDisplay/SwitchLandscape.h on the host: window A's column scan of a decoder's block-linear NV12
// picture, read through a model of the window's fetch (L4T window.c: SCAN_COLUMN makes a panel line a source column and
// a panel pixel a source row, H_DIRECTION and V_DIRECTION walk them back from ADDR_H_OFFSET and ADDR_V_OFFSET, the DDAs
// in 4.12), puts every source pixel where tegra_vic_transform_point does for each IOSurfaceAccelerator quarter turn
// (the mapping tegra_vic_transpose_test holds VIC's own jobs to on the hardware's layout model), at 1:1 pixel for
// pixel; scaled, every sample stays inside the source rectangle; and what window A cannot turn is refused.
#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

#include "../SwitchLandscape.h"
#include "../../TegraVIC/test/TegraVICProbe.h"

namespace {
using namespace native_planes;

class Landscape : public testing::Test {
protected:
    Layer picture;
    switch_gpu_backing_layout layout = {};
    std::vector<uint8_t> memory;
    Image image;

    static uint8_t lumaAt(uint32_t x, uint32_t y)
    {
        return uint8_t(x * 7 + y * 13 + (x >> 4) * (y >> 3));
    }

    static uint8_t chromaAt(uint32_t xB, uint32_t y)
    {
        return uint8_t(xB * 3 + y * 29 + 7);
    }

    void SetUp() override
    {
        picture.kind = kKindNv12Video;
        picture.surface.format = fourcc('4', '2', '0', 'v');
        picture.surface.width = 1280;
        picture.surface.height = 720;
        picture.surface.planes = 2;
        layout.format = picture.surface.format;
        layout.plane_count = 2;
        layout.color_matrix = 2;
        layout.color_range = 1;
        layout.planes[0] = {0, 1280 * 736, 1280, 736, 1280, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        layout.planes[1] = {1280 * 736, 1280 * 368, 640, 368, 1280, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        memory.assign(1280 * (736 + 368), 0);
        const auto &l = layout.planes[0], &c = layout.planes[1];
        for (uint32_t y = 0; y < l.height; ++y)
            for (uint32_t x = 0; x < l.width; ++x)
                memory[l.offset + tgm_layout_offset_of(l.pitch, 1, x, y)] = lumaAt(x, y);
        for (uint32_t y = 0; y < c.height; ++y)
            for (uint32_t xB = 0; xB < c.width * 2; ++xB)
                memory[c.offset + tgm_layout_offset_of(c.pitch, 1, xB, y)] = chromaAt(xB, y);
    }

    // Panel pixel `pixel` of line `line` inside the window: the source (x, y) it reads, from the registers alone.
    void sample(uint32_t line, uint32_t pixel, int64_t &x, int64_t &y) const
    {
        const uint32_t along = (image.hInitial + pixel * (image.dda & 0xffff)) >> 12;
        const uint32_t across = (image.vInitial + line * (image.dda >> 16)) >> 12;
        x = (image.options & native_column_scan::kHDirection) ? int64_t(image.hOffset) - across
                                                              : int64_t(image.hOffset) + across;
        y = (image.options & native_column_scan::kVDirection) ? int64_t(image.vOffset) - along
                                                              : int64_t(image.vOffset) + along;
    }

    uint32_t fetch(int64_t x, int64_t y) const
    {
        const uint8_t yLog2 = uint8_t((image.surfaceKind >> 4) & 7);
        const uint8_t luma =
            memory[image.start[0] + tgm_layout_offset_of(image.lineStride & 0xffff, yLog2, uint32_t(x), uint32_t(y))];
        const uint8_t cb = memory[image.start[1] + tgm_layout_offset_of(image.lineStride >> 16, yLog2,
                                                                        uint32_t(x) & ~1u, uint32_t(y) / 2)];
        return luma | uint32_t(cb) << 8;
    }

    bool admit(Rect src, uint32_t transform, Rect dst, uint32_t cropX = 0, uint32_t cropY = 0)
    {
        return switch_landscape::videoImage(picture, layout, cropX, cropY, src, transform, dst, 720, 1280, image);
    }
};

class EveryQuarterTurn : public Landscape, public testing::WithParamInterface<uint32_t> {};

TEST_P(EveryQuarterTurn, AtOneToOneThePanelIsTheTurnedPicturePixelForPixel)
{
    const uint32_t t = GetParam();
    picture.surface.width = 1264;          // the decoder's picture from (16, 8) in its planes
    const Rect src = {64, 40, 1080, 680};  // the turned picture is 680 x 1080 on the panel
    ASSERT_TRUE(admit(src, t, {20, 100, 680, 1080}, 16, 8));
    EXPECT_EQ(image.dda, 0x10001000u);
    EXPECT_EQ(image.options & (kHFilterOn | kVFilterOn), 0u);
    EXPECT_NE(image.options & native_column_scan::kScanColumn, 0u);
    EXPECT_EQ(image.position, (100u << 16) | 20u);
    EXPECT_EQ(image.size, (1080u << 16) | 680u);
    EXPECT_EQ(image.prescaled, (1080u << 16) | 680u);  // V: the source columns, H: its rows in bytes
    for (uint32_t sy = 0; sy < 680; sy += 3)
        for (uint32_t sx = 0; sx < 1080; sx += 7) {
            uint32_t u = 0, v = 0;  // the destination pixel source pixel (sx, sy) lands on: panel pixel u of line v
            tegra_vic_transform_point(t, 1080, 680, sx, sy, &u, &v);
            int64_t x = 0, y = 0;
            sample(v, u, x, y);
            ASSERT_EQ(x, int64_t(16 + 64 + sx)) << "t " << t << " line " << v << " pixel " << u;
            ASSERT_EQ(y, int64_t(8 + 40 + sy)) << "t " << t << " line " << v << " pixel " << u;
            ASSERT_EQ(fetch(x, y), uint32_t(lumaAt(uint32_t(x), uint32_t(y))) |
                                       uint32_t(chromaAt(uint32_t(x) & ~1u, uint32_t(y) / 2)) << 8);
        }
}

TEST_P(EveryQuarterTurn, ScaledEverySampleStaysInsideTheSourceRectangle)
{
    const uint32_t t = GetParam();
    const Rect src = {0, 0, 1280, 720};
    ASSERT_TRUE(admit(src, t, {0, 0, 480, 853}));  // 1.5x down along the rows, 1.5x down along the columns
    EXPECT_NE(image.options & (kHFilterOn | kVFilterOn), 0u);
    EXPECT_EQ(image.dda, (ddaIncrement(1280, 853, 15) << 16) | ddaIncrement(720, 480, 8));
    for (uint32_t line = 0; line < 853; ++line)
        for (uint32_t pixel = 0; pixel < 480; pixel += 11) {
            int64_t x = 0, y = 0;
            sample(line, pixel, x, y);
            ASSERT_GE(x, 0);
            ASSERT_LT(x, 1280);
            ASSERT_GE(y, 0);
            ASSERT_LT(y, 720);
        }
}

INSTANTIATE_TEST_SUITE_P(Transforms, EveryQuarterTurn, testing::Values(4u, 5u, 6u, 7u));

TEST_F(Landscape, WhatWindowACannotTurnIsRefused)
{
    const Rect src = {0, 0, 1280, 720}, dst = {0, 0, 720, 1280};
    for (uint32_t t : {0u, 1u, 2u, 3u, 8u})
        EXPECT_FALSE(admit(src, t, dst)) << t;
    EXPECT_FALSE(admit({1, 0, 1278, 720}, 7, dst));      // half a chroma pair
    EXPECT_FALSE(admit(src, 7, {1, 0, 720, 1280}));      // off the panel
    EXPECT_FALSE(admit(src, 7, {0, 0, 720 / 9, 1280}));  // past the H DDA's 8.0
    EXPECT_FALSE(admit({0, 0, 1280, 722}, 7, dst));      // past the picture
    EXPECT_TRUE(admit(src, 7, dst));
    picture.surface.width = 4098;
    layout.planes[0] = {0, 4160 * 736, 4160, 736, 4160, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
    layout.planes[1] = {4160 * 736, 4160 * 368, 2080, 368, 4160, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
    EXPECT_FALSE(admit({0, 0, 4098, 720}, 7, {0, 0, 720, 1280}));  // past PLANAR_ROTATION_MAXIMUM_SIZE
    EXPECT_TRUE(admit({0, 0, 4096, 720}, 7, {0, 0, 720, 1280}));
}

// The destination Core Animation detaches is what the turn filled ('422v' on iPadOS 27, portrait), shown whole.
TEST_F(Landscape, ADetachedTurnedDestinationScansThePictureItself)
{
    Layer destination;
    destination.kind = kKindNv16Video;
    destination.surface.format = fourcc('4', '2', '2', 'v');
    destination.surface.width = 720;
    destination.surface.height = 1280;
    destination.src = {0, 0, 720, 1280};
    destination.dst = {0, 0, 720, 1280};
    const Rect turned = {0, 0, 1280, 720};
    Image direct;
    ASSERT_TRUE(switch_landscape::videoImage(picture, layout, 0, 0, turned, 7, destination.dst, 720, 1280, direct));
    ASSERT_TRUE(switch_landscape::turnedVideoImage(destination, layout, 0, 0, 1280, 720, 7, turned, 720, 1280, image));
    EXPECT_EQ(image.options, direct.options);
    EXPECT_EQ(image.prescaled, direct.prescaled);
    EXPECT_EQ(image.hOffset, direct.hOffset);
    EXPECT_EQ(image.start[1], direct.start[1]);
    destination.dst = {0, 160, 720, 960};  // letterboxed: still the whole destination
    EXPECT_TRUE(switch_landscape::turnedVideoImage(destination, layout, 0, 0, 1280, 720, 7, turned, 720, 1280, image));
    destination.src = {0, 0, 720, 1278};  // part of the destination
    EXPECT_FALSE(switch_landscape::turnedVideoImage(destination, layout, 0, 0, 1280, 720, 7, turned, 720, 1280, image));
    destination.src = {0, 0, 720, 1280};
    destination.transform = 4;
    EXPECT_FALSE(switch_landscape::turnedVideoImage(destination, layout, 0, 0, 1280, 720, 7, turned, 720, 1280, image));
    destination.transform = 0;
    layout.chroma_top = layout.chroma_bottom = 3;  // sited where window A does not sample: VIC resamples it
    EXPECT_FALSE(switch_landscape::turnedVideoImage(destination, layout, 0, 0, 1280, 720, 7, turned, 720, 1280, image));
    layout.chroma_top = layout.chroma_bottom = 0;
    layout.format = fourcc('x', '4', '2', '0');
    EXPECT_FALSE(switch_landscape::turnedVideoImage(destination, layout, 0, 0, 1280, 720, 7, turned, 720, 1280, image));
}

TEST_F(Landscape, TheUiOnWindowBIsInFrontOfTheVideo)
{
    EXPECT_EQ(switch_landscape::kUiOnB.layer & 0xff, 0u);  // depth 0: in front
    EXPECT_EQ(switch_landscape::kVideoOnA.layer & 0xff, 1u);
    EXPECT_NE(switch_landscape::kVideoOnA.layer & kBlendBypass, 0u);
    EXPECT_EQ(switch_landscape::kUiOnB.match, kBlendOver);
}
}  // namespace
