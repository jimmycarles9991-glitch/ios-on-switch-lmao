// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_surface_view.h"
#include <gtest/gtest.h>
#include <vector>

TEST(RawSurfaceView, DirectViewsPreserveCanonicalStrideAndHeight)
{
    tgm_layout layout;
    ASSERT_TRUE(tgm_layout_tiled(64, 64, 4, &layout));
    raw::SurfaceView view;
    ASSERT_TRUE(raw::surfaceView(layout, 0, 64, 64, 4, 256, view));
    EXPECT_TRUE(view.direct);
    ASSERT_TRUE(raw::surfaceView(layout, 0, 63, 36, 4, 256, view));
    EXPECT_TRUE(view.direct);
    ASSERT_TRUE(raw::surfaceView(layout, 0, 36, 36, 4, 256, view));
    EXPECT_FALSE(view.direct);
    ASSERT_TRUE(raw::surfaceView(layout, 0, 64, 28, 4, 256, view));
    EXPECT_FALSE(view.direct);
}

TEST(RawSurfaceView, OffsetsAreCoordinatesInCpuPitchNotTiledOffsets)
{
    tgm_layout layout;
    ASSERT_TRUE(tgm_layout_tiled(64, 64, 4, &layout));
    raw::SurfaceView view;
    ASSERT_TRUE(raw::surfaceView(layout, 16 * 320 + 8 * 4, 32, 32, 4, 320, view));
    EXPECT_EQ(view.x, 8u);
    EXPECT_EQ(view.y, 16u);
    EXPECT_FALSE(view.direct);
    EXPECT_FALSE(raw::surfaceView(layout, 16 * 320 + 1, 32, 32, 4, 320, view));
    EXPECT_FALSE(raw::surfaceView(layout, 60 * 320, 32, 8, 4, 320, view));
    EXPECT_FALSE(raw::surfaceView(layout, 0, 65, 64, 4, 320, view));
    EXPECT_FALSE(raw::surfaceView(layout, 0, 64, 64, 8, 512, view));
    EXPECT_FALSE(raw::surfaceView(layout, UINT64_MAX, 1, 1, 4, 320, view));
}

TEST(RawSurfaceView, CroppedWritebackPreservesEveryNeighbor)
{
    tgm_layout layout;
    ASSERT_TRUE(tgm_layout_tiled(64, 64, 4, &layout));
    raw::SurfaceView view;
    ASSERT_TRUE(raw::surfaceView(layout, 9 * 256 + 12 * 4, 36, 28, 4, 256, view));
    std::vector<uint32_t> original(64 * 64), image(36 * 28), result(64 * 64);
    for (uint32_t i = 0; i < original.size(); ++i)
        original[i] = i ^ 0x12345678u;
    std::vector<uint8_t> tiled(layout.bytes);
    tgm_layout_write(&layout, tiled.data(), 0, 0, 64, 64, reinterpret_cast<const uint8_t *>(original.data()), 256);
    tgm_layout_read(&layout, tiled.data(), view.x, view.y, 36, 28, reinterpret_cast<uint8_t *>(image.data()), 36 * 4);
    for (uint32_t y = 0; y < 28; ++y)
        for (uint32_t x = 0; x < 36; ++x) {
            EXPECT_EQ(image[y * 36 + x], original[(y + view.y) * 64 + x + view.x]);
            image[y * 36 + x] ^= UINT32_MAX;
        }
    tgm_layout_write(&layout, tiled.data(), view.x, view.y, 36, 28, reinterpret_cast<const uint8_t *>(image.data()),
                     36 * 4);
    tgm_layout_read(&layout, tiled.data(), 0, 0, 64, 64, reinterpret_cast<uint8_t *>(result.data()), 256);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x) {
            const bool inside = x >= view.x && x < view.x + 36 && y >= view.y && y < view.y + 28;
            EXPECT_EQ(result[y * 64 + x], original[y * 64 + x] ^ (inside ? UINT32_MAX : 0));
        }
}

TEST(RawSurfaceView, RecognizesIdsAndOverlappingCpuAliases)
{
    EXPECT_TRUE(raw::surfaceBackingAliases(7, 0x1000, 0x1000, 7, 0x8000, 0x1000));
    EXPECT_TRUE(raw::surfaceBackingAliases(7, 0x1000, 0x1000, 0, 0x1800, 0x1000));
    EXPECT_TRUE(raw::surfaceBackingAliases(0, 0x1800, 0x1000, 7, 0x1000, 0x1000));
    EXPECT_FALSE(raw::surfaceBackingAliases(0, 0x1000, 0x1000, 0, 0x2000, 0x1000));
    EXPECT_FALSE(raw::surfaceBackingAliases(8, 0x1000, 0x1000, 7, 0x8000, 0x1000));
}
