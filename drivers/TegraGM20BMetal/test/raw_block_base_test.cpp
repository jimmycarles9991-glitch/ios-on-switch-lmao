// SPDX-License-Identifier: MIT
// raw_block_base.h: which no-copy bases a block-compressed texture is its caller's memory at, and which go through a pool
// image the copy engine fills, over the 32 512-byte bases of a 16 KiB page that HW Test gpu.astc.layouts wraps (bits 9-13).
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../raw/fx/fx_image.h"
#include "../raw/runtime/raw_block_base.h"

namespace {

struct Shape {
    const char *name;
    uint32_t blockW, blockH, bpp, width, height, levels, layers;
    uint64_t align;  // NIL's: max(level 0's tile, 4096)
};

const Shape kShapes[] = {
    {"ASTC 4x4 128x128 (gpu.astc.layouts, ui.compressed_image)", 4, 4, 16, 128, 128, 1, 1, 4096},
    {"ASTC 4x4 128x128, full chain", 4, 4, 16, 128, 128, 8, 1, 4096},
    {"ASTC 4x4 1024x1024, full chain", 4, 4, 16, 1024, 1024, 11, 1, 8192},
    {"ASTC 4x4 512x256, 2 levels, 3 layers", 4, 4, 16, 512, 256, 2, 3, 4096},
    {"ASTC 8x8 256x256", 8, 8, 16, 256, 256, 1, 1, 4096},
    {"ASTC 12x12 84x84", 12, 12, 16, 84, 84, 1, 1, 4096},
    {"BC1 64x64, full chain", 4, 4, 8, 64, 64, 7, 1, 4096},
    {"BC1 4096x4096", 4, 4, 8, 4096, 4096, 1, 1, 8192},
};

std::vector<uintptr_t> inPlaceBases(const raw::fx::MipChain &layout)
{
    std::vector<uintptr_t> bases;
    for (uintptr_t base = 0; base < 16384; base += 512)
        if (raw::blockImageInPlace(0x1'2340'0000 + base, layout))
            bases.push_back(base);
    return bases;
}

TEST(BlockBase, InPlaceOnlyAtNilsAlignment)
{
    for (const Shape &s : kShapes) {
        SCOPED_TRACE(s.name);
        raw::fx::MipChain layout;
        ASSERT_TRUE(
            raw::fx::blockChainLayout(s.width, s.height, s.levels, s.layers, s.blockW, s.blockH, s.bpp, layout));
        ASSERT_EQ(layout.align, s.align);
        std::vector<uintptr_t> want;
        for (uintptr_t base = 0; base < 16384; base += s.align)
            want.push_back(base);
        EXPECT_EQ(inPlaceBases(layout), want);
    }
}

TEST(BlockBase, EveryBaseWithBit9To11SetGoesThroughThePoolImage)
{
    for (const Shape &s : kShapes) {
        SCOPED_TRACE(s.name);
        raw::fx::MipChain layout;
        ASSERT_TRUE(
            raw::fx::blockChainLayout(s.width, s.height, s.levels, s.layers, s.blockW, s.blockH, s.bpp, layout));
        for (uintptr_t base = 0; base < 16384; base += 512)
            if (base & 0xe00)
                EXPECT_FALSE(raw::blockImageInPlace(0x1'2340'0000 + base, layout)) << base;
    }
}

TEST(BlockBase, GoesByTheImagesAlignmentNotThePages)
{
    raw::fx::MipChain small, large;
    ASSERT_TRUE(raw::fx::blockChainLayout(128, 128, 1, 1, 4, 4, 16, small));
    ASSERT_TRUE(raw::fx::blockChainLayout(1024, 1024, 11, 1, 4, 4, 16, large));
    EXPECT_TRUE(raw::blockImageInPlace(0x1'0000'5000, small));
    EXPECT_FALSE(raw::blockImageInPlace(0x1'0000'5000, large));
    EXPECT_TRUE(raw::blockImageInPlace(0x1'0000'6000, large));
    raw::fx::MipChain none;
    EXPECT_FALSE(raw::blockImageInPlace(0, none));
}

}  // namespace
