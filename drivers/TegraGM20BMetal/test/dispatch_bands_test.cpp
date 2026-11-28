// SPDX-License-Identifier: MIT
// dispatch_bands.h on the host: a long dispatch's pieces cover its grid once along its longest axis, a
// piece count follows the budget, and the piece's base reaches the root table's workgroup base, which the stage compile
// (switch_compile) builds every compute shader to read.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/band_rates.h"
#include "drivers/TegraGM20BMetal/dispatch_bands.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(DispatchPieces, ThePiecesCoverTheGridOnce)
{
    const std::array<std::array<uint32_t, 3>, 4> grids = {{{1000, 1, 1}, {3, 517, 1}, {7, 5, 33}, {1, 1, 1}}};
    for (const auto &grid : grids) {
        const uint8_t axis = gm20b::pieceAxis(grid);
        for (uint32_t n = 1; n <= std::min<uint32_t>(grid[axis], gm20b::kMaxBands); n++) {
            std::vector<int> hits(grid[axis]);
            for (uint32_t i = 0; i < n; i++) {
                const gm20b::DispatchPiece p = gm20b::dispatchPiece(grid, axis, i, n);
                for (uint8_t a = 0; a < 3; a++)
                    if (a != axis) {
                        EXPECT_EQ(p.base[a], 0u);
                        EXPECT_EQ(p.groups[a], grid[a]);
                    }
                EXPECT_GT(p.groups[axis], 0u);
                for (uint32_t g = p.base[axis]; g < p.base[axis] + p.groups[axis]; g++)
                    hits[g]++;
            }
            for (int h : hits)
                EXPECT_EQ(h, 1);
        }
    }
}

TEST(DispatchPieces, TheLongestAxisIsCut)
{
    EXPECT_EQ(gm20b::pieceAxis({1000, 1, 1}), 0);
    EXPECT_EQ(gm20b::pieceAxis({3, 517, 1}), 1);
    EXPECT_EQ(gm20b::pieceAxis({7, 5, 33}), 2);
}

// An 11.5 ms dispatch in twelve; never more pieces than groups on the axis, the command buffer's bands or kMaxBands
TEST(DispatchPieces, TheCountFollowsTheBudget)
{
    EXPECT_EQ(gm20b::pieceCount(900'000, TGM_APP_BAND_NS, 100, gm20b::kBandsPerCommandBuffer), 1u);
    EXPECT_EQ(gm20b::pieceCount(11'500'000, TGM_APP_BAND_NS, 100, gm20b::kBandsPerCommandBuffer), 12u);
    EXPECT_EQ(gm20b::pieceCount(11'500'000, TGM_APP_BAND_NS, 5, gm20b::kBandsPerCommandBuffer), 5u);
    EXPECT_EQ(gm20b::pieceCount(11'500'000, TGM_APP_BAND_NS, 1, gm20b::kBandsPerCommandBuffer), 1u);
    EXPECT_EQ(gm20b::pieceCount(11'500'000, TGM_APP_BAND_NS, 100, 3), 3u);
    EXPECT_EQ(gm20b::pieceCount(500'000'000, TGM_APP_BAND_NS, 10'000, gm20b::kBandsPerCommandBuffer), gm20b::kMaxBands);
    EXPECT_EQ(gm20b::pieceCount(11'500'000, 0, 100, gm20b::kBandsPerCommandBuffer), 1u);
    EXPECT_EQ(gm20b::pieceCount(248'000'000, TGM_APP_BAND_NS, 10'000, gm20b::kMaxTileBands, gm20b::kMaxTileBands),
              248u);
    EXPECT_EQ(gm20b::pieceCount(10'000, TGM_APP_BAND_NS, 1000, gm20b::kMaxTileBands, gm20b::kMaxTileBands, true), 1u);
    EXPECT_EQ(gm20b::pieceCount(248'000'000, TGM_APP_BAND_NS, 10'000, gm20b::kMaxTileBands, gm20b::kMaxTileBands, true),
              gm20b::kMaxUnmeasuredBands);
    EXPECT_EQ(gm20b::pieceCount(8'000'000, TGM_APP_BAND_NS, 1000, gm20b::kMaxTileBands, gm20b::kMaxTileBands, false,
                                500'000),
              3u);
    EXPECT_EQ(gm20b::pieceCount(10'000, 0, 1000, gm20b::kMaxTileBands, gm20b::kMaxTileBands, true), 1u);
}

// A kernel priced by its instructions until a dispatch of it is measured, then by its time a thread
TEST(DispatchPieces, AMeasuredKernelIsCutByItsTime)
{
    gm20b::BandRate rate;
    const uint64_t threads = 1024 * 1024;
    const uint64_t staticNs = gm20b::workNs(gm20b::laneWork(threads, 40));
    EXPECT_EQ(gm20b::pieceCount(gm20b::estimatedNs(rate, threads, staticNs), TGM_APP_BAND_NS, 4096, 64), 1u);
    gm20b::learn(rate, 6'000'000, threads);
    EXPECT_EQ(gm20b::pieceCount(gm20b::estimatedNs(rate, threads, staticNs), TGM_APP_BAND_NS, 4096, 64), 6u);
}

// A grid past the QMD's grid fields (2^31 - 1 across, 65535 down and deep) is launched in slices within them that cover
// each threadgroup once, each based where it starts within its piece
TEST(DispatchPieces, QmdSlicesCoverAGridPastTheQmdOnce)
{
    for (const std::array<uint32_t, 3> &groups :
         {std::array<uint32_t, 3>{3, 70000, 1}, {1, 65535, 65536}, {2, 131071, 3}, {0x80000000u, 1, 1}, {5, 5, 5}}) {
        const gm20b::DispatchPiece piece{{7, 11, 13}, groups};
        const uint64_t count = gm20b::qmdSliceCount({groups[0], groups[1], groups[2]});
        std::array<uint64_t, 3> covered{};
        for (uint64_t i = 0; i < count; i++) {
            const gm20b::DispatchPiece s = gm20b::qmdSlice(piece, i);
            for (uint8_t a = 0; a < 3; a++) {
                EXPECT_GT(s.groups[a], 0u);
                EXPECT_LE(s.groups[a], gm20b::kQmdGroups[a]);
                EXPECT_EQ((s.base[a] - piece.base[a]) % gm20b::kQmdGroups[a], 0u);
                EXPECT_LE(s.base[a] - piece.base[a] + s.groups[a], groups[a]);
            }
            covered[0] += uint64_t(s.groups[0]) * s.groups[1] * s.groups[2];
        }
        EXPECT_EQ(covered[0], uint64_t(groups[0]) * groups[1] * groups[2]);
    }
    EXPECT_EQ(gm20b::qmdSliceCount({5, 5, 5}), 1u);
    EXPECT_EQ(gm20b::qmdSliceCount({3, 70000, 1}), 2u);
    EXPECT_EQ(gm20b::qmdSliceCount({1, 65535, 65536}), 2u);
    EXPECT_EQ(gm20b::qmdSliceCount({1, 0xffffffffull, 1}), gm20b::kMaxQmdSlices);
    EXPECT_EQ(gm20b::kMaxQmdSlices, 65537u);
}

// A launch writes the piece's base and the whole grid's count into the root table's first words, where the kernel's
// compile reads its workgroup base
TEST(DispatchPieces, ThePiecesBaseIsTheKernelsWorkgroupBase)
{
    const std::string compute = SourceText("../compute.cpp");
    const std::string launch = Body(compute, "bool ComputeEncoder::launch(");
    EXPECT_THAT(launch, HasSubstr("rootWords(r, piece.base.data(), grid.data(),"));
    EXPECT_THAT(launch, HasSubstr("qmdWords(*state_, piece.groups.data(),"));
    const std::string words = Body(compute, "void ComputeEncoder::rootWords(");
    EXPECT_THAT(words, HasSubstr("root[a] = base[a];"));
    EXPECT_THAT(words, HasSubstr("root[3 + a] = groups[a];"));
    EXPECT_THAT(Body(compute, "bool ComputeEncoder::qmdWords("), HasSubstr("qmd[kQmdRasterWidth] = groups[0];"));
}

TEST(DispatchPieces, TheStageCompileReadsTheWorkgroupBaseFromTheRootTable)
{
    const std::string root = SourceText("../../../third_party/mesa/src/nouveau/switch_compile/nvk_root_table.h");
    EXPECT_THAT(
        root, HasSubstr("struct {\n         uint32_t base_group[3];\n         uint32_t group_count[3];\n      } cs;"));
    EXPECT_THAT(SourceText("../../../third_party/mesa/src/nouveau/switch_compile/nvk_shader_lower.c"),
                HasSubstr(".has_base_workgroup_id = mesa_shader_stage_is_compute(nir->info.stage),"));
    EXPECT_THAT(SourceText("../../../third_party/mesa/src/nouveau/switch_compile/nvk_nir_lower_descriptors.c"),
                HasSubstr("return lower_sysval_to_root_table(b, intrin, cs.base_group, ctx);"));
}

}  // namespace
