// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_texture_cpu_range.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <vector>

namespace {
using raw::TextureCpuRegion;

raw::TextureCpuShape shape(uint32_t w, uint32_t h, uint8_t bpp)
{
    return {w, h, 1, 1, w * bpp, 0, 0, bpp, 0, false, false, nullptr, nullptr, 0};
}

// Enumerate the memory's block/GOB/16-byte-line order, then map each logical byte back to that sequence.
std::vector<uint64_t> addresses(const raw::fx::MipLevel &level, uint32_t bpp)
{
    const uint32_t width = level.width * bpp, height = level.height, depth = level.depth;
    std::vector<uint64_t> result(uint64_t(width) * height * depth, UINT64_MAX);
    uint64_t address = level.offset;
    const uint32_t gobRows = 1u << level.yLog2, blockDepth = 1u << level.zLog2;
    for (uint32_t blockZ = 0; blockZ < level.planes; blockZ += blockDepth)
        for (uint32_t blockY = 0; blockY < level.rows; blockY += 8 * gobRows)
            for (uint32_t blockX = 0; blockX < level.rowBytes; blockX += 64)
                for (uint32_t localZ = 0; localZ < blockDepth; ++localZ)
                    for (uint32_t gobY = 0; gobY < gobRows; ++gobY)
                        for (uint32_t halfX = 0; halfX < 2; ++halfX)
                            for (uint32_t pairY = 0; pairY < 4; ++pairY)
                                for (uint32_t quarterX = 0; quarterX < 2; ++quarterX)
                                    for (uint32_t parityY = 0; parityY < 2; ++parityY)
                                        for (uint32_t byte = 0; byte < 16; ++byte, ++address) {
                                            const uint32_t x = blockX + halfX * 32 + quarterX * 16 + byte;
                                            const uint32_t y = blockY + gobY * 8 + pairY * 2 + parityY,
                                                           z = blockZ + localZ;
                                            if (x < width && y < height && z < depth)
                                                result[(uint64_t(z) * height + y) * width + x] = address;
                                        }
    return result;
}

void compare(const raw::TextureCpuShape &s, const raw::fx::MipChain &layout, uint32_t level, uint32_t slice,
             const std::vector<uint64_t> &map, const uint64_t origin[3], const uint64_t size[3])
{
    raw::TextureCpuRange range;
    ASSERT_EQ(raw::textureCpuRange(s, origin, size, level, slice, range), TextureCpuRegion::Native);
    uint64_t first = UINT64_MAX, last = 0;
    const auto &lv = layout.level[level];
    for (uint64_t z = origin[2]; z < origin[2] + size[2]; ++z)
        for (uint64_t y = origin[1]; y < origin[1] + size[1]; ++y)
            for (uint64_t x = origin[0] * s.bpp; x < (origin[0] + size[0]) * s.bpp; ++x) {
                const auto address = map[(z * lv.height + y) * lv.width * s.bpp + x] + slice * layout.arrayStride;
                first = std::min(first, address);
                last = std::max(last, address);
            }
    EXPECT_EQ(range.offset, first);
    EXPECT_EQ(range.bytes, last - first + 1);
}

TEST(TextureCpuRange, ExhaustiveSmallRectanglesCrossGobRowsAndColumns)
{
    auto s = shape(19, 10, 4);
    s.tiled = true;
    raw::fx::MipChain layout;
    ASSERT_TRUE(raw::fx::mipChainLayout(s.width, s.height, 1, s.bpp, layout));
    const auto map = addresses(layout.level[0], s.bpp);
    for (uint64_t y = 0; y < s.height; ++y)
        for (uint64_t h = 1; h <= s.height - y; ++h)
            for (uint64_t x = 0; x < s.width; ++x)
                for (uint64_t w = 1; w <= s.width - x; ++w) {
                    const uint64_t origin[] = {x, y, 0}, size[] = {w, h, 1};
                    compare(s, layout, 0, 0, map, origin, size);
                }
}

TEST(TextureCpuRange, VolumeBoxesCrossGobAndDepthBlockBoundaries)
{
    for (bool view2d : {false, true}) {
        auto s = shape(19, 10, 4);
        s.volume = true;
        s.tiled = true;
        s.depth = 5;
        s.blockDepth = view2d ? 0 : 2;
        raw::fx::MipChain layout;
        ASSERT_TRUE(raw::fx::volumeLayout(s.width, s.height, s.depth, 1, s.bpp, view2d, layout));
        const auto map = addresses(layout.level[0], s.bpp);
        for (uint64_t z = 0; z < s.depth; ++z)
            for (uint64_t d = 1; d <= s.depth - z; ++d)
                for (uint64_t y : {0u, 1u, 7u, 8u, 9u})
                    for (uint64_t endY : {1u, 2u, 8u, 9u, 10u})
                        for (uint64_t x : {0u, 3u, 4u, 7u, 8u, 15u, 16u, 18u})
                            for (uint64_t endX : {1u, 4u, 8u, 16u, 17u, 19u}) {
                                if (endY <= y || endX <= x)
                                    continue;
                                const uint64_t origin[] = {x, y, z}, size[] = {endX - x, endY - y, d};
                                compare(s, layout, 0, 0, map, origin, size);
                            }
    }
}

TEST(TextureCpuRange, AwkwardRectanglesCrossTallGobBlocksAtEveryTexelSize)
{
    for (uint8_t bpp : {1, 2, 4, 8, 16}) {
        auto s = shape(131, 137, bpp);
        s.tiled = true;
        raw::fx::MipChain layout;
        ASSERT_TRUE(raw::fx::mipChainLayout(s.width, s.height, 1, bpp, layout));
        const auto map = addresses(layout.level[0], bpp);
        for (uint64_t y : {0u, 7u, 8u, 15u, 16u, 63u, 64u, 127u, 128u, 136u})
            for (uint64_t h : {1u, 2u, 9u})
                for (uint64_t x : {0u, 3u, 4u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 129u, 130u})
                    for (uint64_t w : {1u, 2u, 5u, 17u}) {
                        if (x + w > s.width || y + h > s.height)
                            continue;
                        const uint64_t origin[] = {x, y, 0}, size[] = {w, h, 1};
                        compare(s, layout, 0, 0, map, origin, size);
                    }
    }
}

TEST(TextureCpuRange, MipsAndLayersDoNotAcquireTheWholeParentImage)
{
    auto s = shape(129, 87, 4);
    s.tiled = true;
    raw::fx::MipChain layout;
    ASSERT_TRUE(raw::fx::mipChainLayout(s.width, s.height, 7, s.bpp, layout));
    s.chain = &layout;
    for (uint32_t level = 0; level < layout.levels; ++level) {
        const auto map = addresses(layout.level[level], s.bpp);
        const uint64_t origin[] = {0, 0, 0}, size[] = {layout.level[level].width, layout.level[level].height, 1};
        compare(s, layout, level, 0, map, origin, size);
    }
    s.chain = nullptr;
    s.layers = 6;
    ASSERT_TRUE(raw::fx::arrayLayout(s.width, s.height, s.layers, 1, s.bpp, layout));
    const auto map = addresses(layout.level[0], s.bpp);
    for (uint32_t slice = 0; slice < s.layers; ++slice) {
        const uint64_t origin[] = {15, 7, 0}, size[] = {3, 3, 1};
        compare(s, layout, 0, slice, map, origin, size);
    }
}

TEST(TextureCpuRange, AViewOfAMipmappedArraysLevelZeroStridesByTheChain)
{
    auto s = shape(64, 64, 4);
    s.tiled = true;
    s.layers = 3;
    raw::fx::MipChain chain, one;
    ASSERT_TRUE(raw::fx::arrayLayout(64, 64, 3, 4, 4, chain));
    ASSERT_TRUE(raw::fx::arrayLayout(64, 64, 3, 1, 4, one));
    ASSERT_NE(chain.arrayStride, one.arrayStride);
    s.layerBytes = chain.arrayStride;
    const uint64_t origin[] = {0, 0, 0}, size[] = {1, 1, 1};
    raw::TextureCpuRange range;
    ASSERT_EQ(raw::textureCpuRange(s, origin, size, 0, 2, range), TextureCpuRegion::Native);
    EXPECT_EQ(range.offset, 2 * chain.arrayStride);
}

TEST(TextureCpuRange, CroppedViewUsesOwningImportLayoutWithItsOriginalCoordinates)
{
    tgm_layout layout;
    ASSERT_TRUE(tgm_layout_tiled(131, 89, 4, &layout));
    auto s = shape(9, 9, 4);
    s.tiled = true;
    s.surfaceX = 31;
    s.surfaceY = 15;
    s.importLayout = &layout;
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::mipChainLayout(layout.width, layout.height, 1, 4, c));
    const auto map = addresses(c.level[0], 4);
    const uint64_t origin[] = {1, 1, 0}, size[] = {7, 7, 1};
    raw::TextureCpuRange range;
    ASSERT_EQ(raw::textureCpuRange(s, origin, size, 0, 0, range), TextureCpuRegion::Native);
    uint64_t first = UINT64_MAX, last = 0;
    for (unsigned y = 16; y < 23; ++y)
        for (unsigned x = 128; x < 156; ++x) {
            const auto address = map[y * layout.width * 4 + x];
            first = std::min(first, address);
            last = std::max(last, address);
        }
    EXPECT_EQ(range.offset, first);
    EXPECT_EQ(range.bytes, last - first + 1);
    EXPECT_EQ(s.importLayout, &layout);
    EXPECT_EQ(s.surfaceX, 31u);
    EXPECT_EQ(s.surfaceY, 15u);
}

TEST(TextureCpuRange, LinearRegionBoundsRowsWithoutAcquiringParentPitchPadding)
{
    auto s = shape(15, 7, 4);
    s.pitch = 128;
    const uint64_t origin[] = {1, 2, 0}, size[] = {3, 2, 1};
    raw::TextureCpuRange range;
    ASSERT_EQ(raw::textureCpuRange(s, origin, size, 0, 0, range), TextureCpuRegion::Native);
    EXPECT_EQ(range.offset, 260u);
    EXPECT_EQ(range.bytes, 140u);
    EXPECT_LT(range.offset + range.bytes, uint64_t(s.pitch) * s.height);
}

TEST(TextureCpuRange, RejectsOverflowAndOutOfBoundsBeforeReturningAUsableRange)
{
    auto s = shape(19, 10, 4);
    raw::TextureCpuRange range{1, 2};
    uint64_t origin[] = {18, 9, 0}, size[] = {1, 1, 1};
    EXPECT_EQ(raw::textureCpuRange(s, origin, size, 0, 0, range), TextureCpuRegion::Native);
    size[0] = 2;
    EXPECT_EQ(raw::textureCpuRange(s, origin, size, 0, 0, range), TextureCpuRegion::Invalid);
    EXPECT_EQ(range.bytes, 0u);
    origin[0] = UINT64_MAX;
    EXPECT_EQ(raw::textureCpuRange(s, origin, size, 0, 0, range), TextureCpuRegion::Invalid);
    origin[0] = 0;
    size[0] = 1;
    origin[2] = 1;
    EXPECT_EQ(raw::textureCpuRange(s, origin, size, 0, 0, range), TextureCpuRegion::Invalid);
    origin[2] = 0;
    s.pitch = 3;
    EXPECT_EQ(raw::textureCpuRange(s, origin, size, 0, 0, range), TextureCpuRegion::Invalid);
    size[2] = 0;
    EXPECT_EQ(raw::textureCpuRange(s, origin, size, 0, 0, range), TextureCpuRegion::Empty);
}
}  // namespace
