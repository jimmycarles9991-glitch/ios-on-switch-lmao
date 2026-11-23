// SPDX-License-Identifier: MIT
// texel_rows.h: the parts of a block-linear surface's rows a texel buffer's bytes lie in. Byte b of the
// buffer is the surface's byte offset + b of rows bytesPerRow apart; the layout holds each row's texels alone. Over every
// shape below, each byte the layout holds is in exactly one part, at its place in the buffer, and no part reaches a byte
// past a row's texels, past the last row or outside the range: the copies back never write what the buffer does not hold.
#include <gtest/gtest.h>

#include <vector>

#include "../texel_rows.h"

namespace {

constexpr uint64_t kNone = ~uint64_t(0);

TEST(TexelRows, EveryByteTheLayoutHoldsIsInOnePartAtItsPlace)
{
    for (const uint32_t bpp : {1u, 4u, 16u})
        for (const uint32_t width : {3u, 33u})
            for (const uint32_t height : {1u, 5u})
                for (const uint32_t pad : {0u, 1u, 48u}) {
                    tgm_layout l = {};
                    ASSERT_TRUE(tgm_layout_tiled(width, height, bpp, &l));
                    const uint32_t bytesPerRow = width * bpp + pad;
                    const uint64_t texels = uint64_t(width) * bpp, surface = uint64_t(bytesPerRow) * (height + 1);
                    for (uint64_t offset = 0; offset < std::min<uint64_t>(surface, 2 * bytesPerRow + 32); offset += 16)
                        for (const uint64_t ask : {uint64_t(1), uint64_t(7), uint64_t(16), uint64_t(17),
                                                   uint64_t(bytesPerRow - 1), uint64_t(bytesPerRow),
                                                   uint64_t(bytesPerRow + 1), uint64_t(2 * bytesPerRow + 5), surface}) {
                            const uint64_t bytes = std::min(ask, surface - offset);
                            const gm20b::TexelRowsParts parts = gm20b::texelRowsParts(l, offset, bytesPerRow, bytes);
                            std::vector<uint64_t> at(bytes, kNone);  // the surface byte each buffer byte moves to
                            uint64_t moved = 0;
                            for (uint32_t k = 0; k < parts.count; k++) {
                                const gm20b::TexelRowsPart &p = parts.part[k];
                                ASSERT_TRUE(p.unit && p.unit <= 16 && !(p.unit & (p.unit - 1)));
                                ASSERT_EQ((p.column | p.bytes | bytesPerRow) % p.unit, 0u)
                                    << "the copy engine's element divides where and how much it moves";
                                for (uint64_t r = 0; r < p.rows; r++)
                                    for (uint64_t c = 0; c < p.bytes; c++) {
                                        const uint64_t b = p.at + r * bytesPerRow + c;
                                        ASSERT_LT(b, bytes) << "no part reaches past the range";
                                        ASSERT_EQ(at[b], kNone) << "byte " << b << " in one part";
                                        at[b] = (p.row + r) * bytesPerRow + p.column + c;
                                        moved++;
                                    }
                            }
                            uint64_t held = 0;
                            for (uint64_t b = 0; b < bytes; b++) {
                                const uint64_t linear = offset + b;
                                const bool stored = linear / bytesPerRow < height && linear % bytesPerRow < texels;
                                held += stored;
                                ASSERT_EQ(at[b], stored ? linear : kNone) << "byte " << b << " of " << bytes << " from "
                                                                          << offset << ", rows " << bytesPerRow;
                            }
                            ASSERT_EQ(moved, held);
                            ASSERT_EQ(parts.held, held);
                            ASSERT_LE(parts.count, 3u);
                        }
                }
}

// 300 RGBA8 texels from byte 160 of a 64x16 RGBA8 surface of rows 320 bytes apart: the last 96 bytes of row 0's texels,
// rows 1 to 3 whole and the first 80 of row 4's, in 16-byte elements; the 64 bytes of padding of rows 0 to 3 in none
TEST(TexelRows, ARangeIsAPartOfItsFirstRowItsWholeRowsAndAPartOfItsLast)
{
    tgm_layout l = {};
    ASSERT_TRUE(tgm_layout_tiled(64, 16, 4, &l));
    const gm20b::TexelRowsParts parts = gm20b::texelRowsParts(l, 160, 320, 1200);
    ASSERT_EQ(parts.count, 3u);
    const gm20b::TexelRowsPart want[3] = {{0, 1, 160, 96, 16, 0}, {1, 3, 0, 256, 16, 160}, {4, 1, 0, 80, 16, 1120}};
    for (uint32_t k = 0; k < 3; k++) {
        const gm20b::TexelRowsPart &p = parts.part[k], &w = want[k];
        EXPECT_EQ(p.row, w.row) << k;
        EXPECT_EQ(p.rows, w.rows) << k;
        EXPECT_EQ(p.column, w.column) << k;
        EXPECT_EQ(p.bytes, w.bytes) << k;
        EXPECT_EQ(p.unit, w.unit) << k;
        EXPECT_EQ(p.at, w.at) << k;
    }
    EXPECT_EQ(parts.held, 944u) << "1200 less 4 rows' padding of 64 bytes";
    EXPECT_TRUE(gm20b::texelRowsAddressable(parts));
}

}  // namespace
