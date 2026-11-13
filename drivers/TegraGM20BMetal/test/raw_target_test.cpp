// SPDX-License-Identifier: MIT
// raw/runtime/raw_target.h on the host. A pass into a chain level writes through SET_COLOR_TARGET_*: the render hardware
// addresses a block-linear target from its base, its WIDTH (rounded up to whole GOBs: the row stride) and its block height,
// one GOB wide. Every texel it writes must be the byte raw/fx's layout has for that texel (fx::texelOffset: the layout the
// texture header samples and the 2D engine writes, which the device's self-test checks on the GPU), or the levels the raw
// path renders would land beside where the next draw samples them.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

#include "../raw/runtime/raw_target.h"

namespace {

using raw::ColorTargetDesc;
using raw::fx::MipChain;

// The nine SET_COLOR_TARGET_* values the raw path writes for `d`
std::vector<uint32_t> targetWords(const ColorTargetDesc &d)
{
    const switch_gm20b_target k = raw::kitTarget(d);
    std::vector<uint32_t> words(SWITCH_GM20B_COLOR_TARGET_WORDS);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_color_target(&p, 0, &k, false);
    return std::vector<uint32_t>(words.begin() + 1, words.begin() + 10);
}

// Tegra block-linear addressing of a surface with blocks one GOB (64 B x 8 rows) wide and 1 << blockHeight GOBs tall, rows of
// rowBytes: blocks row by row, GOBs top to bottom in a block, TegraColor order inside a GOB
uint64_t blockLinearOffset(uint32_t rowBytes, uint32_t blockHeight, uint32_t xB, uint32_t y)
{
    const uint32_t rowsPerBlock = 8u << blockHeight, gobsPerRow = rowBytes / 64;
    const uint64_t block = uint64_t(y / rowsPerBlock) * gobsPerRow + xB / 64;
    return block * (512ull << blockHeight) + uint64_t((y % rowsPerBlock) / 8) * 512 + raw::fx::gobOffset(xB, y);
}

struct Size {
    uint32_t w, h, levels;
};

const Size kSizes[] = {{2, 2, 2},       {3, 5, 3},       {776, 24, 10},    {180, 180, 8}, {120, 120, 7},
                       {1280, 720, 11}, {720, 1280, 11}, {1024, 1, 11},    {1, 1024, 11}, {2048, 1536, 12},
                       {97, 3, 7},      {33, 65, 7},     {1366, 1024, 11}, {256, 8, 9},   {8, 256, 9},
                       {300, 200, 9},   {640, 480, 10},  {85, 85, 7},      {17, 9, 5},    {4096, 4096, 13}};

TEST(RawTarget, PitchWordsAsBefore)
{
    ColorTargetDesc d;
    d.va = 0x2345678000ull;  // the GPU's 40-bit address space
    d.pitch = 5120;
    d.height = 720;
    d.bpp = 4;
    d.rtFormat = 0xcf;
    EXPECT_EQ(targetWords(d), (std::vector<uint32_t>{0x23, 0x45678000, 5120, 720, 0xcf, 0x1000, 1, 0, 0}));
}

TEST(RawTarget, OnlyLaterLevels)
{
    MipChain c;
    ASSERT_TRUE(raw::fx::mipChainLayout(1280, 720, 11, 4, c));
    ColorTargetDesc d;
    EXPECT_FALSE(raw::chainLevelDesc(c, 0x100000, 0, 0xcf, d));
    EXPECT_FALSE(raw::chainLevelDesc(c, 0x100000, 11, 0xcf, d));
    EXPECT_FALSE(raw::chainLevelDesc(c, 0x100000, 1, 0, d));
    EXPECT_TRUE(raw::chainLevelDesc(c, 0x100000, 10, 0xcf, d));
}

// NVK's words for the level, and the hardware's addressing of them equal to raw/fx's layout of the level, texel by texel
TEST(RawTarget, ChainLevelAddressesTheLayout)
{
    const uint64_t chainVa = 0x2000000000ull;  // a chain's base (fx::kChainAlign aligned)
    unsigned checked = 0;
    for (uint32_t bpp : {4u, 8u})
        for (const Size &s : kSizes) {
            MipChain c;
            ASSERT_TRUE(raw::fx::mipChainLayout(s.w, s.h, s.levels, bpp, c)) << s.w << "x" << s.h;
            for (uint32_t l = 1; l < c.levels; l++) {
                const raw::fx::MipLevel &lv = c.level[l];
                ColorTargetDesc d;
                ASSERT_TRUE(raw::chainLevelDesc(c, chainVa, l, 0xcf, d));
                const std::vector<uint32_t> w = targetWords(d);
                const uint64_t va = uint64_t(w[0]) << 32 | w[1];
                EXPECT_EQ(va, chainVa + lv.offset);
                EXPECT_EQ(va % 512, 0u) << "a level starts on a GOB";
                EXPECT_EQ(w[2] * bpp, lv.rowBytes) << "WIDTH is the row stride in texels";
                EXPECT_EQ(lv.rowBytes % 64, 0u);
                EXPECT_GE(lv.rowBytes, lv.width * bpp);
                EXPECT_EQ(w[3], lv.height);
                EXPECT_EQ(w[4], 0xcfu);
                EXPECT_EQ(w[5], uint32_t(lv.yLog2) << 4) << "ONE_GOB wide, the level's block height, BLOCKLINEAR";
                EXPECT_EQ(w[6], 1u);
                EXPECT_EQ(uint64_t(w[7]) * 4, c.bytes);
                EXPECT_EQ(w[8], 0u);
                // the rows the target spans stay inside the level and the chain
                const uint64_t end = lv.offset + uint64_t(lv.rowBytes) * lv.rows;
                EXPECT_LE(end, c.bytes);
                if (l + 1 < c.levels)
                    EXPECT_LE(end, c.level[l + 1].offset);
                const uint32_t gobsPerRow = (w[2] * bpp + 63) / 64;
                const uint32_t stepX = lv.width > 64 ? 7 : 1, stepY = lv.height > 64 ? 5 : 1;
                for (uint32_t y = 0; y < lv.height; y += stepY)
                    for (uint32_t x = 0; x < lv.width; x += stepX)
                        for (uint32_t b = 0; b < bpp; b += bpp - 1) {
                            const uint32_t xB = x * bpp + b;
                            const uint64_t hw = (va - chainVa) + blockLinearOffset(gobsPerRow * 64, lv.yLog2, xB, y);
                            const uint64_t fx = raw::fx::texelOffset(c, l, xB, y);
                            if (hw != fx) {
                                ADD_FAILURE()
                                    << s.w << "x" << s.h << " bpp " << bpp << " level " << l << " texel (" << x << ","
                                    << y << ") byte " << b << ": target " << hw << ", layout " << fx;
                                return;
                            }
                            checked++;
                        }
            }
        }
    std::printf("%u texel bytes addressed alike\n", checked);
}

// A Shared mipmapped texture's CPU access to a later level: every texel byte written lands where fx::texelOffset has it (the
// layout the texture header samples), nothing outside the region is touched, and reading the region back gives the bytes
TEST(RawTarget, CpuLevelCopyFollowsTheLayout)
{
    unsigned checked = 0;
    for (bool fermi : {false, true})
        for (uint32_t bpp : {2u, 4u, 8u})
            for (const Size &s : kSizes) {
                if (uint64_t(s.w) * s.h > 1u << 21)
                    continue;
                MipChain c;
                ASSERT_TRUE(raw::fx::mipChainLayout(s.w, s.h, s.levels, bpp, c));
                std::vector<uint8_t> chain(c.bytes, 0xee);
                for (uint32_t l = 1; l < c.levels; l++) {
                    const raw::fx::MipLevel &lv = c.level[l];
                    const uint32_t x = lv.width / 3, y = lv.height / 4, w = lv.width - x, h = lv.height - y;
                    const size_t row = size_t(w) * bpp + 5;
                    std::vector<uint8_t> src(row * h), back(row * h, 0);
                    for (size_t i = 0; i < src.size(); i++)
                        src[i] = uint8_t(i * 131 + l * 7 + 1);
                    raw::fx::writeLevel(c, chain.data(), l, x, y, w, h, src.data(), row, fermi);
                    for (uint32_t r = 0; r < h; r++)
                        for (uint32_t b = 0; b < w * bpp; b++) {
                            const uint64_t at = raw::fx::texelOffset(c, l, x * bpp + b, y + r, fermi);
                            if (chain[at] != src[r * row + b]) {
                                ADD_FAILURE() << s.w << "x" << s.h << " level " << l << " (" << b << "," << r << ")";
                                return;
                            }
                            checked++;
                        }
                    raw::fx::readLevel(c, chain.data(), l, x, y, w, h, back.data(), row, fermi);
                    for (uint32_t r = 0; r < h; r++)
                        ASSERT_EQ(0, memcmp(back.data() + r * row, src.data() + r * row, size_t(w) * bpp));
                }
                size_t untouched = 0;
                for (uint8_t b : chain)
                    untouched += b == 0xee;
                size_t written = 0;
                for (uint32_t l = 1; l < c.levels; l++) {
                    const raw::fx::MipLevel &lv = c.level[l];
                    written += size_t(lv.width - lv.width / 3) * (lv.height - lv.height / 4) * bpp;
                }
                EXPECT_GE(untouched, c.bytes - written) << "bytes outside the regions were written";
            }
    std::printf("%u texel bytes copied by the CPU where the layout has them\n", checked);
}

// bits [lo, hi] of a 256-bit texture header
uint64_t headerField(const uint32_t w[8], unsigned hi, unsigned lo)
{
    uint64_t v = 0;
    for (unsigned b = hi + 1; b-- > lo;)
        v = v << 1 | (w[b / 32] >> (b % 32) & 1);
    return v;
}

// What NVK reports for Core Animation's backdrop chains (raw/fx/test/niloracle.c, formats 44 and 97, 776x24x10): each level's
// offset and row pitch, and the image's size
struct NilLevel {
    uint64_t offset;
    uint32_t rowPitch;
};

const NilLevel kNilBgra8[] = {{0, 3136},     {100352, 1600}, {125952, 832}, {132608, 448}, {136192, 192},
                              {137728, 128}, {138752, 64},   {139264, 64},  {139776, 64},  {140288, 64}};
const NilLevel kNilRgba16f[] = {{0, 6208},     {198656, 3136}, {248832, 1600}, {261632, 832}, {268288, 384},
                                {271360, 192}, {272896, 128},  {273920, 64},   {274432, 64},  {274944, 64}};

// The chain's texture header names level 0 alone (address, block height, extent, last level); the texture unit finds level n
// by NIL's rule from it: levels packed in order, rows whole GOBs, level 0's block height clamped to the level. The target a
// pass renders that level through must be the same bytes, and the header of the level alone (Device::levelTic, what a
// framebuffer fetch from the level reads) must be the chain's own header narrowed to that level.
TEST(RawTarget, BackdropChainLevelsAreWhereTheHeaderSamplesThem)
{
    const uint64_t chainVa = 0x2000000000ull;
    for (const auto &[bpp, nil, bytes] :
         {std::tuple{4u, kNilBgra8, 143360ull}, std::tuple{8u, kNilRgba16f, 278528ull}}) {
        raw::fx::MipChain c;
        ASSERT_TRUE(raw::fx::mipChainLayout(776, 24, 10, bpp, c));
        EXPECT_EQ(c.bytes, bytes);
        uint32_t chain[8];
        raw::fx::mipChainTic(c, chainVa, 0, false, 0, c.levels, chain);
        const uint64_t base = headerField(chain, 79, 64) << 32 | headerField(chain, 63, 41) << 9;
        const uint32_t width = uint32_t(headerField(chain, 143, 128)) + 1,
                       height = uint32_t(headerField(chain, 175, 160)) + 1;
        const uint32_t y0 = uint32_t(headerField(chain, 101, 99)), last = uint32_t(headerField(chain, 127, 124));
        EXPECT_EQ(base, chainVa);
        EXPECT_EQ(last, c.levels - 1);
        uint64_t at = 0;
        for (uint32_t l = 0; l <= last; l++) {
            const uint32_t lw = std::max(1u, width >> l), lh = std::max(1u, height >> l);
            uint32_t y = 0;
            while ((1u << y) < (lh + 7) / 8)
                y++;
            y = std::min(y, y0);
            const uint32_t rowBytes = (lw * bpp + 63) / 64 * 64, rows = (lh + (8u << y) - 1) / (8u << y) * (8u << y);
            EXPECT_EQ(at, nil[l].offset) << "bpp " << bpp << " level " << l;
            EXPECT_EQ(rowBytes, nil[l].rowPitch) << "bpp " << bpp << " level " << l;
            if (l) {
                raw::ColorTargetDesc d;
                ASSERT_TRUE(raw::chainLevelDesc(c, chainVa, l, 0xcf, d));
                const std::vector<uint32_t> w = targetWords(d);
                EXPECT_EQ(uint64_t(w[0]) << 32 | w[1], chainVa + at) << "bpp " << bpp << " level " << l;
                EXPECT_EQ(w[2] * bpp, rowBytes) << "bpp " << bpp << " level " << l;
                EXPECT_EQ(w[3], lh) << "bpp " << bpp << " level " << l;
                EXPECT_EQ(w[5] >> 4 & 0xf, y) << "bpp " << bpp << " level " << l;
                uint32_t alone[8];
                raw::fx::mipChainTic(c, chainVa, 0, false, l, 1, alone);
                EXPECT_EQ(headerField(alone, 227, 224), l);
                EXPECT_EQ(headerField(alone, 231, 228), l);
                for (int k = 0; k < 7; k++)
                    EXPECT_EQ(alone[k], chain[k]) << "bpp " << bpp << " level " << l << " word " << k;
            }
            at += uint64_t(rowBytes) * rows;
        }
    }
}

}  // namespace
