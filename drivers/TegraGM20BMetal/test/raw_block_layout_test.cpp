// SPDX-License-Identifier: MIT
// raw/fx's block-compressed image (fx::blockChainLayout, the layout object a no-copy texture's memory is filled through) is
// NIL's. kNil holds what NVK answers for a VK_IMAGE_TILING_OPTIMAL 2D image of that format, size and levels
// (raw/fx/test/niloracle.c over the NVK host twin and the mock GM20B: vkGetImageMemoryRequirements' size and each level's
// vkGetImageSubresourceLayout offset, row pitch and size). The CPU copies (fx::writeElements, readElements) put every byte
// where TegraGM20BLayout.h's TegraColor GOB addressing has it.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../raw/fx/fx_image.h"
#include "TegraGM20BLayout.h"

namespace {

struct NilLevel {
    uint64_t offset, rowPitch, bytes;
};

struct NilImage {
    uint32_t blockW, blockH, bpp, width, height, levels;
    uint64_t bytes;
    std::vector<NilLevel> level;
};

// niloracle <dylib> <157 ASTC_4x4 | 183 ASTC_12x12 | 171 ASTC_8x8 | 133 BC1_RGBA | 165 ASTC_6x6 | 179 ASTC_10x10> WxHxL ...
const NilImage kNil[] = {
    {4,
     4,
     16,
     1024,
     1024,
     11,
     1400832,
     {{0, 4096, 1048576},
      {1048576, 2048, 262144},
      {1310720, 1024, 65536},
      {1376256, 512, 16384},
      {1392640, 256, 4096},
      {1396736, 128, 1024},
      {1397760, 64, 512},
      {1398272, 64, 512},
      {1398784, 64, 512},
      {1399296, 64, 512},
      {1399808, 64, 512}}},
    {4,
     4,
     16,
     84,
     84,
     7,
     20480,
     {{0, 384, 12288},
      {12288, 192, 3072},
      {15360, 128, 1024},
      {16384, 64, 512},
      {16896, 64, 512},
      {17408, 64, 512},
      {17920, 64, 512}}},
    {4,
     4,
     16,
     130,
     37,
     8,
     16384,
     {{0, 576, 9216},
      {9216, 320, 2560},
      {11776, 128, 1024},
      {12800, 64, 512},
      {13312, 64, 512},
      {13824, 64, 512},
      {14336, 64, 512},
      {14848, 64, 512}}},
    {4, 4, 16, 1, 1, 1, 4096, {{0, 64, 512}}},
    {4, 4, 16, 4, 4, 1, 4096, {{0, 64, 512}}},
    {4,
     4,
     16,
     2048,
     16,
     12,
     135168,
     {{0, 8192, 65536},
      {65536, 4096, 32768},
      {98304, 2048, 16384},
      {114688, 1024, 8192},
      {122880, 512, 4096},
      {126976, 256, 2048},
      {129024, 128, 1024},
      {130048, 64, 512},
      {130560, 64, 512},
      {131072, 64, 512},
      {131584, 64, 512},
      {132096, 64, 512}}},
    {12,
     12,
     16,
     100,
     70,
     7,
     8192,
     {{0, 192, 1536},
      {1536, 128, 1024},
      {2560, 64, 512},
      {3072, 64, 512},
      {3584, 64, 512},
      {4096, 64, 512},
      {4608, 64, 512}}},
    {12,
     12,
     16,
     1000,
     1000,
     10,
     237568,
     {{0, 1344, 172032},
      {172032, 704, 45056},
      {217088, 384, 12288},
      {229376, 192, 3072},
      {232448, 128, 1024},
      {233472, 64, 512},
      {233984, 64, 512},
      {234496, 64, 512},
      {235008, 64, 512},
      {235520, 64, 512}}},
    {8,
     8,
     16,
     64,
     64,
     7,
     4096,
     {{0, 128, 1024},
      {1024, 64, 512},
      {1536, 64, 512},
      {2048, 64, 512},
      {2560, 64, 512},
      {3072, 64, 512},
      {3584, 64, 512}}},
    {8,
     8,
     16,
     200,
     13,
     8,
     12288,
     {{0, 448, 3584},
      {3584, 256, 2048},
      {5632, 128, 1024},
      {6656, 64, 512},
      {7168, 64, 512},
      {7680, 64, 512},
      {8192, 64, 512},
      {8704, 64, 512}}},
    {4, 4, 8, 30, 20, 5, 4096, {{0, 64, 512}, {512, 64, 512}, {1024, 64, 512}, {1536, 64, 512}, {2048, 64, 512}}},
    {4,
     4,
     8,
     256,
     256,
     9,
     49152,
     {{0, 512, 32768},
      {32768, 256, 8192},
      {40960, 128, 2048},
      {43008, 64, 512},
      {43520, 64, 512},
      {44032, 64, 512},
      {44544, 64, 512},
      {45056, 64, 512},
      {45568, 64, 512}}},
    {6,
     6,
     16,
     1000,
     600,
     10,
     466944,
     {{0, 2688, 344064},
      {344064, 1344, 86016},
      {430080, 704, 22528},
      {452608, 384, 6144},
      {458752, 192, 1536},
      {460288, 128, 1024},
      {461312, 64, 512},
      {461824, 64, 512},
      {462336, 64, 512},
      {462848, 64, 512}}},
    {10,
     10,
     16,
     300,
     300,
     9,
     24576,
     {{0, 512, 16384},
      {16384, 256, 4096},
      {20480, 128, 1024},
      {21504, 64, 512},
      {22016, 64, 512},
      {22528, 64, 512},
      {23040, 64, 512},
      {23552, 64, 512},
      {24064, 64, 512}}},
};

TEST(BlockChainLayout, IsNils)
{
    for (const NilImage &n : kNil) {
        raw::fx::MipChain c;
        ASSERT_TRUE(raw::fx::blockChainLayout(n.width, n.height, n.levels, 1, n.blockW, n.blockH, n.bpp, c))
            << n.width << "x" << n.height;
        EXPECT_EQ(c.bytes, n.bytes) << n.width << "x" << n.height << " blocks " << n.blockW << "x" << n.blockH;
        for (uint32_t l = 0; l < n.levels; l++) {
            const raw::fx::MipLevel &lv = c.level[l];
            EXPECT_EQ(lv.offset, n.level[l].offset) << n.width << "x" << n.height << " level " << l;
            EXPECT_EQ(lv.rowBytes, n.level[l].rowPitch) << n.width << "x" << n.height << " level " << l;
            EXPECT_EQ(uint64_t(lv.rowBytes) * lv.rows, n.level[l].bytes)
                << n.width << "x" << n.height << " level " << l;
        }
    }
}

TEST(BlockChainLayout, ArraysStrideByLevelZerosTile)
{
    raw::fx::MipChain one, three;
    ASSERT_TRUE(raw::fx::blockChainLayout(84, 84, 7, 1, 4, 4, 16, one));
    ASSERT_TRUE(raw::fx::blockChainLayout(84, 84, 7, 3, 4, 4, 16, three));
    EXPECT_EQ(three.arrayStride, one.arrayStride);
    EXPECT_EQ(three.arrayStride % (512ull << three.yLog2), 0u);
    EXPECT_GE(three.bytes, three.arrayStride * 3);
}

TEST(BlockChainLayout, RefusesWhatItDoesNotLayOut)
{
    raw::fx::MipChain c;
    EXPECT_FALSE(raw::fx::blockChainLayout(0, 4, 1, 1, 4, 4, 16, c));
    EXPECT_FALSE(raw::fx::blockChainLayout(4, 4, 4, 1, 4, 4, 16, c));  // more levels than the full chain
    EXPECT_FALSE(raw::fx::blockChainLayout(4, 4, 1, 0, 4, 4, 16, c));
    EXPECT_FALSE(raw::fx::blockChainLayout(4, 4, 1, 1, 4, 4, 12, c));
}

// Every byte of every level and layer where TegraGM20BLayout.h's addressing has it, whole GOBs and edge GOBs alike, and
// back out unchanged, including a rectangle that starts and ends inside GOBs
TEST(BlockElements, LandWhereTheLayoutAddressesThem)
{
    for (const auto &[width, height, blockW, bpp] :
         {std::tuple{300u, 70u, 4u, 16u}, std::tuple{100u, 100u, 12u, 16u}, std::tuple{30u, 20u, 4u, 8u}}) {
        raw::fx::MipChain c;
        ASSERT_TRUE(raw::fx::blockChainLayout(width, height, 4, 2, blockW, blockW, bpp, c));
        std::vector<uint8_t> memory(c.bytes, 0);
        for (uint32_t layer = 0; layer < 2; layer++)
            for (uint32_t l = 0; l < 4; l++) {
                const raw::fx::MipLevel &lv = c.level[l];
                const size_t row = size_t(lv.width) * bpp;
                std::vector<uint8_t> src(row * lv.height), back(row * lv.height);
                for (size_t i = 0; i < src.size(); i++)
                    src[i] = uint8_t(i * 7 + l * 31 + layer * 101 + (i >> 8));
                raw::fx::writeElements(c, memory.data(), layer, l, 0, 0, lv.width, lv.height, src.data(), row);
                for (uint32_t y = 0; y < lv.height; y++)
                    for (uint32_t xB = 0; xB < row; xB++)
                        ASSERT_EQ(memory[layer * c.arrayStride + lv.offset +
                                         tgm_layout_offset_of(lv.rowBytes, lv.yLog2, xB, y)],
                                  src[y * row + xB])
                            << width << "x" << height << " layer " << layer << " level " << l << " (" << xB << ", " << y
                            << ")";
                raw::fx::readElements(c, memory.data(), layer, l, 0, 0, lv.width, lv.height, back.data(), row);
                EXPECT_EQ(back, src);
                if (lv.width > 3 && lv.height > 9) {
                    const uint32_t w = lv.width - 3, h = lv.height - 9;
                    std::vector<uint8_t> part(size_t(w) * bpp * h, 0xa5), got(part.size());
                    raw::fx::writeElements(c, memory.data(), layer, l, 1, 5, w, h, part.data(), size_t(w) * bpp);
                    raw::fx::readElements(c, memory.data(), layer, l, 1, 5, w, h, got.data(), size_t(w) * bpp);
                    EXPECT_EQ(got, part);
                    EXPECT_EQ(
                        memory[layer * c.arrayStride + lv.offset + tgm_layout_offset_of(lv.rowBytes, lv.yLog2, 0, 0)],
                        src[0]);
                }
            }
    }
}

}  // namespace
