// SPDX-License-Identifier: MIT
// fx's block-compressed image (fx::blockChainLayout, the layout object a no-copy texture's memory is filled through) is
// NIL's. kNil holds what NVK answers for a VK_IMAGE_TILING_OPTIMAL 2D image of that format, size and levels
// (fx/test/niloracle.c over the NVK host twin and the mock GM20B: vkGetImageMemoryRequirements' size and each level's
// vkGetImageSubresourceLayout offset, row pitch and size). The CPU copies (fx::writeElements, readElements) put every byte
// where TegraGM20BLayout.h's TegraColor GOB addressing has it.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../fx/fx_image.h"
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
        gm20b::fx::MipChain c;
        ASSERT_TRUE(gm20b::fx::blockChainLayout(n.width, n.height, n.levels, 1, n.blockW, n.blockH, n.bpp, c))
            << n.width << "x" << n.height;
        EXPECT_EQ(c.bytes, n.bytes) << n.width << "x" << n.height << " blocks " << n.blockW << "x" << n.blockH;
        for (uint32_t l = 0; l < n.levels; l++) {
            const gm20b::fx::MipLevel &lv = c.level[l];
            EXPECT_EQ(lv.offset, n.level[l].offset) << n.width << "x" << n.height << " level " << l;
            EXPECT_EQ(lv.rowBytes, n.level[l].rowPitch) << n.width << "x" << n.height << " level " << l;
            EXPECT_EQ(uint64_t(lv.rowBytes) * lv.rows, n.level[l].bytes)
                << n.width << "x" << n.height << " level " << l;
        }
    }
}

// NVK's answers for VK_IMAGE_TYPE_3D optimal images of BC1_RGBA (133, 8-byte blocks), ASTC_4x4 (157) and ASTC_10x10
// (179) (m2vf-shapes2 probe/nil/niloracle3d, niloracle.c over a 3D extent, the NVK host twin of x1-allon-key): the
// image's size and each level's offset, row pitch, depth pitch (one plane of blocks) and size. Metal's BC and ASTC 3D
// textures hold blocks one plane deep.
struct NilVolume {
    uint32_t blockW, bpp, width, height, depth, levels;
    uint64_t bytes;
    std::vector<std::array<uint64_t, 4>> level;
};

const NilVolume kNilVolumes[] = {
    {4, 8, 16, 16, 4, 3, 4096, {{0, 64, 512, 2048}, {2048, 64, 512, 1024}, {3072, 64, 512, 512}}},
    {4,
     8,
     64,
     32,
     12,
     5,
     24576,
     {{0, 128, 1024, 16384},
      {16384, 64, 512, 4096},
      {20480, 64, 512, 2048},
      {22528, 64, 512, 512},
      {23040, 64, 512, 512}}},
    {4,
     8,
     100,
     60,
     7,
     4,
     40960,
     {{0, 256, 4096, 32768}, {32768, 128, 1024, 4096}, {36864, 64, 512, 512}, {37376, 64, 512, 512}}},
    {4, 8, 8, 8, 1, 1, 4096, {{0, 64, 512, 512}}},
    {4, 8, 4, 4, 16, 3, 16384, {{0, 64, 512, 8192}, {8192, 64, 512, 4096}, {12288, 64, 512, 2048}}},
    {4,
     16,
     64,
     32,
     12,
     5,
     49152,
     {{0, 256, 2048, 32768},
      {32768, 128, 1024, 8192},
      {40960, 64, 512, 2048},
      {43008, 64, 512, 512},
      {43520, 64, 512, 512}}},
    {4,
     16,
     100,
     60,
     7,
     4,
     69632,
     {{0, 448, 7168, 57344}, {57344, 256, 2048, 8192}, {65536, 128, 1024, 1024}, {66560, 64, 512, 512}}},
    {10, 16, 50, 30, 5, 3, 12288, {{0, 128, 1024, 8192}, {8192, 64, 512, 1024}, {9216, 64, 512, 512}}},
};

TEST(BlockVolumeLayout, IsNils)
{
    for (const NilVolume &n : kNilVolumes) {
        gm20b::fx::MipChain c;
        ASSERT_TRUE(gm20b::fx::volumeLayout(n.width, n.height, n.depth, n.levels, n.bpp, false, c, n.blockW, n.blockW))
            << n.width << "x" << n.height << "x" << n.depth;
        EXPECT_EQ(c.bytes, n.bytes) << n.width << "x" << n.height << "x" << n.depth << " blocks " << n.blockW;
        for (uint32_t l = 0; l < n.levels; l++) {
            const gm20b::fx::MipLevel &lv = c.level[l];
            const uint64_t plane = uint64_t(lv.rowBytes) * lv.rows;
            EXPECT_EQ(lv.offset, n.level[l][0]) << n.width << "x" << n.height << "x" << n.depth << " level " << l;
            EXPECT_EQ(lv.rowBytes, n.level[l][1]) << n.width << "x" << n.height << "x" << n.depth << " level " << l;
            EXPECT_EQ(plane, n.level[l][2]) << n.width << "x" << n.height << "x" << n.depth << " level " << l;
            EXPECT_EQ(plane * lv.planes, n.level[l][3])
                << n.width << "x" << n.height << "x" << n.depth << " level " << l;
        }
    }
}

// A 3D image's planes of blocks move through the volume copies as elements: every byte back out as it went in, at the
// offset volumeOffset gives it
TEST(BlockVolumeLayout, PlanesOfBlocksRoundTrip)
{
    gm20b::fx::MipChain c;
    ASSERT_TRUE(gm20b::fx::volumeLayout(100, 60, 7, 3, 16, false, c, 4, 4));
    std::vector<uint8_t> memory(c.bytes, 0);
    for (uint32_t l = 0; l < 3; l++) {
        const gm20b::fx::MipLevel &lv = c.level[l];
        const size_t row = size_t(lv.width) * 16, plane = row * lv.height;
        std::vector<uint8_t> src(plane * lv.depth), back(src.size());
        for (size_t i = 0; i < src.size(); i++)
            src[i] = uint8_t(i * 13 + l * 29 + (i >> 9));
        const uint32_t origin[3] = {0, 0, 0}, size[3] = {lv.width, lv.height, lv.depth};
        gm20b::fx::writeVolume(c, memory.data(), l, origin, size, src.data(), row, plane);
        for (uint32_t z = 0; z < lv.depth; z++)
            for (uint32_t y = 0; y < lv.height; y++)
                ASSERT_EQ(memory[gm20b::fx::volumeOffset(c, l, 17, y, z)], src[z * plane + y * row + 17])
                    << "level " << l << " plane " << z << " row " << y;
        gm20b::fx::readVolume(c, memory.data(), l, origin, size, back.data(), row, plane);
        EXPECT_EQ(back, src) << "level " << l;
    }
}

TEST(BlockChainLayout, ArraysStrideByLevelZerosTile)
{
    gm20b::fx::MipChain one, three;
    ASSERT_TRUE(gm20b::fx::blockChainLayout(84, 84, 7, 1, 4, 4, 16, one));
    ASSERT_TRUE(gm20b::fx::blockChainLayout(84, 84, 7, 3, 4, 4, 16, three));
    EXPECT_EQ(three.arrayStride, one.arrayStride);
    EXPECT_EQ(three.arrayStride % (512ull << three.yLog2), 0u);
    EXPECT_GE(three.bytes, three.arrayStride * 3);
}

TEST(BlockChainLayout, RefusesWhatItDoesNotLayOut)
{
    gm20b::fx::MipChain c;
    EXPECT_FALSE(gm20b::fx::blockChainLayout(0, 4, 1, 1, 4, 4, 16, c));
    EXPECT_FALSE(gm20b::fx::blockChainLayout(4, 4, 4, 1, 4, 4, 16, c));  // more levels than the full chain
    EXPECT_FALSE(gm20b::fx::blockChainLayout(4, 4, 1, 0, 4, 4, 16, c));
    EXPECT_FALSE(gm20b::fx::blockChainLayout(4, 4, 1, 1, 4, 4, 12, c));
}

// Every byte of every level and layer where TegraGM20BLayout.h's addressing has it, whole GOBs and edge GOBs alike, and
// back out unchanged, including a rectangle that starts and ends inside GOBs
TEST(BlockElements, LandWhereTheLayoutAddressesThem)
{
    for (const auto &[width, height, blockW, bpp] :
         {std::tuple{300u, 70u, 4u, 16u}, std::tuple{100u, 100u, 12u, 16u}, std::tuple{30u, 20u, 4u, 8u}}) {
        gm20b::fx::MipChain c;
        ASSERT_TRUE(gm20b::fx::blockChainLayout(width, height, 4, 2, blockW, blockW, bpp, c));
        std::vector<uint8_t> memory(c.bytes, 0);
        for (uint32_t layer = 0; layer < 2; layer++)
            for (uint32_t l = 0; l < 4; l++) {
                const gm20b::fx::MipLevel &lv = c.level[l];
                const size_t row = size_t(lv.width) * bpp;
                std::vector<uint8_t> src(row * lv.height), back(row * lv.height);
                for (size_t i = 0; i < src.size(); i++)
                    src[i] = uint8_t(i * 7 + l * 31 + layer * 101 + (i >> 8));
                gm20b::fx::writeElements(c, memory.data(), layer, l, 0, 0, lv.width, lv.height, src.data(), row);
                for (uint32_t y = 0; y < lv.height; y++)
                    for (uint32_t xB = 0; xB < row; xB++)
                        ASSERT_EQ(memory[layer * c.arrayStride + lv.offset +
                                         tgm_layout_offset_of(lv.rowBytes, lv.yLog2, xB, y)],
                                  src[y * row + xB])
                            << width << "x" << height << " layer " << layer << " level " << l << " (" << xB << ", " << y
                            << ")";
                gm20b::fx::readElements(c, memory.data(), layer, l, 0, 0, lv.width, lv.height, back.data(), row);
                EXPECT_EQ(back, src);
                if (lv.width > 3 && lv.height > 9) {
                    const uint32_t w = lv.width - 3, h = lv.height - 9;
                    std::vector<uint8_t> part(size_t(w) * bpp * h, 0xa5), got(part.size());
                    gm20b::fx::writeElements(c, memory.data(), layer, l, 1, 5, w, h, part.data(), size_t(w) * bpp);
                    gm20b::fx::readElements(c, memory.data(), layer, l, 1, 5, w, h, got.data(), size_t(w) * bpp);
                    EXPECT_EQ(got, part);
                    EXPECT_EQ(
                        memory[layer * c.arrayStride + lv.offset + tgm_layout_offset_of(lv.rowBytes, lv.yLog2, 0, 0)],
                        src[0]);
                }
            }
    }
}

}  // namespace
