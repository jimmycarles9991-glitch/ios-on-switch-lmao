// SPDX-License-Identifier: MIT
// Where Metal's blits between RGB10A8_2P_XR10 textures one of which a descriptor made place each side's bytes
// (xr_copy.h), against an M4 Max's under macOS 27.2.1 (m2v-textures13 probes/c1copies/RESULT.txt)
#include <gtest/gtest.h>

#include <tuple>
#include <utility>
#include <vector>

#include "../xr_copy.h"

namespace {

// Where region row `j` of a copy from a descriptor's (0, 0) starts in its stored alpha rows
std::pair<uint32_t, uint32_t> alphaRowStart(uint32_t width, uint32_t height, uint32_t j)
{
    const gm20b::XrCopySide d = gm20b::xrDescriptorSide(width, height, 1, false, false);
    const gm20b::XrPlace p = gm20b::xrPlace(d, uint64_t(j) * d.pitch);
    EXPECT_TRUE(p.mapped) << width << " row " << j;
    return {p.x, p.y};
}

std::vector<gm20b::XrPiece> rowPieces(const gm20b::XrCopySide &s, uint64_t from, const gm20b::XrCopySide &d,
                                      uint64_t to, uint32_t bytes)
{
    std::vector<gm20b::XrPiece> out;
    gm20b::xrCopyRow(s, from, d, to, bytes, [&](const gm20b::XrPiece &p) {
        out.push_back(p);
    });
    return out;
}

}  // namespace

// A region row j of alpha starts at the stored texel the M4 Max's blit reached: 8 and 17 wide at (0, 2j), 32 at (0, 4j),
// 33 at (0,0), (32,2), (16,5), (0,8), 129 at (0,0), (80,3), (16,7), (96,10)
TEST(GM20BXrCopy, ADescriptorsAlphaRowsLieWhereAnM4MaxsBlitAddressesThem)
{
    using Start = std::pair<uint32_t, uint32_t>;
    for (uint32_t j = 0; j < 4; j++) {
        EXPECT_EQ(alphaRowStart(8, 16, j), Start(0, 2 * j));
        EXPECT_EQ(alphaRowStart(17, 16, j), Start(0, 2 * j));
        EXPECT_EQ(alphaRowStart(32, 16, j), Start(0, 4 * j));
    }
    const Start w33[] = {{0, 0}, {32, 2}, {16, 5}, {0, 8}}, w129[] = {{0, 0}, {80, 3}, {16, 7}, {96, 10}};
    for (uint32_t j = 0; j < 4; j++) {
        EXPECT_EQ(alphaRowStart(33, 16, j), w33[j]) << j;
        EXPECT_EQ(alphaRowStart(129, 16, j), w129[j]) << j;
    }
}

// Words 17 wide (68 bytes a row stored 80 apart, addressed 64 apart): region row 1 starts at stored texel (16, 0), its
// next 12 bytes are that row's padding, read as 0, and it goes on at stored row 1's texel 0
TEST(GM20BXrCopy, ADescriptorsWordRowsRunOnThroughItsPadding)
{
    const gm20b::XrCopySide d = gm20b::xrDescriptorSide(17, 4, 1, true, true),
                            s = gm20b::xrSurfaceSide(16384, 128, true);
    const std::vector<gm20b::XrPiece> row = rowPieces(d, 64, s, 128, 64);
    ASSERT_EQ(row.size(), 3u);
    EXPECT_TRUE(row[0].from.mapped && row[1].to.mapped && row[2].from.mapped);
    EXPECT_FALSE(row[1].from.mapped) << "padding";
    EXPECT_EQ(std::make_tuple(row[0].from.x, row[0].from.y, row[0].bytes), std::make_tuple(16u, 0u, 4u));
    EXPECT_EQ(row[1].bytes, 12u);
    EXPECT_EQ(std::make_tuple(row[2].from.x, row[2].from.y, row[2].bytes), std::make_tuple(0u, 1u, 48u));
    EXPECT_EQ(row[2].to.at, 128u + 16) << "the surface's bytes run on in order";
}

// A 2D array's alpha that runs past a slice lands in the next one, slices ru128(rows x height) apart: 17x17 at 640,
// 16x9 and 33x5 at 256
TEST(GM20BXrCopy, AlphaPastASliceRunsIntoTheNext)
{
    const gm20b::XrCopySide a = gm20b::xrDescriptorSide(17, 17, 2, false, false);
    EXPECT_EQ(a.sliceBytes, 640u);
    EXPECT_EQ(gm20b::xrDescriptorSide(16, 9, 2, false, false).sliceBytes, 256u);
    EXPECT_EQ(gm20b::xrDescriptorSide(33, 5, 2, false, false).sliceBytes, 256u);
    const gm20b::XrPlace pad = gm20b::xrPlace(a, 32 * 17), next = gm20b::xrPlace(a, 640);
    EXPECT_FALSE(pad.mapped) << "between the slices";
    EXPECT_EQ(pad.run, 640u - 32 * 17);
    ASSERT_TRUE(next.mapped);
    EXPECT_EQ(std::make_tuple(next.x, next.y, next.z), std::make_tuple(0u, 0u, 1u));
    EXPECT_FALSE(gm20b::xrPlace(a, 2 * 640).mapped) << "past the last slice";
}

// A descriptor under four texels wide is addressed 16 bytes a row, texel for texel, and as a source gives its row 0 alone
// (every other row reads 0); as a destination it takes every row
TEST(GM20BXrCopy, ANarrowDescriptorGivesItsRowZeroAloneAsASource)
{
    const gm20b::XrCopySide from = gm20b::xrDescriptorSide(2, 8, 1, true, true),
                            to = gm20b::xrDescriptorSide(2, 8, 1, true, false);
    EXPECT_EQ(from.pitch, 16u);
    EXPECT_TRUE(gm20b::xrPlace(from, 4).mapped);
    EXPECT_FALSE(gm20b::xrPlace(from, 16).mapped);
    const gm20b::XrPlace row1 = gm20b::xrPlace(to, 16 + 4);
    ASSERT_TRUE(row1.mapped);
    EXPECT_EQ(std::make_tuple(row1.x, row1.y), std::make_tuple(1u, 1u));
}

// Rows that map alike join one block (a 2D copy); a row a piece of which lies elsewhere stays rows of its own
TEST(GM20BXrCopy, RowsThatMapAlikeAreOneBlock)
{
    const gm20b::XrCopySide s = gm20b::xrSurfaceSide(16384, 128, true);
    std::vector<gm20b::XrBlock> blocks;
    gm20b::xrCopyBlocks(s, 0, 0, gm20b::xrDescriptorSide(16, 8, 1, true, false), 0, 0, 16, 8,
                        [&](const gm20b::XrBlock &b) {
                            blocks.push_back(b);
                        });
    ASSERT_EQ(blocks.size(), 1u) << "16 wide: words texel for texel";
    EXPECT_EQ(blocks[0].rows, 8u);
    blocks.clear();
    gm20b::xrCopyBlocks(s, 0, 0, gm20b::xrDescriptorSide(17, 8, 1, true, false), 0, 0, 16, 8,
                        [&](const gm20b::XrBlock &b) {
                            blocks.push_back(b);
                        });
    EXPECT_GT(blocks.size(), 8u) << "17 wide: each row past the first splits at a stored row's end";
}
