// SPDX-License-Identifier: MIT
// raw/runtime/raw_dispatch_bands.h on the host: a long dispatch's pieces cover its grid once along its longest axis, a
// piece count follows the budget, and the piece's base reaches the root table's workgroup base.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_band_rates.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_dispatch_bands.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(DispatchPieces, ThePiecesCoverTheGridOnce)
{
    const std::array<std::array<uint32_t, 3>, 4> grids = {{{1000, 1, 1}, {3, 517, 1}, {7, 5, 33}, {1, 1, 1}}};
    for (const auto &grid : grids) {
        const uint8_t axis = raw::pieceAxis(grid);
        for (uint32_t n = 1; n <= std::min<uint32_t>(grid[axis], raw::kMaxBands); n++) {
            std::vector<int> hits(grid[axis]);
            for (uint32_t i = 0; i < n; i++) {
                const raw::DispatchPiece p = raw::dispatchPiece(grid, axis, i, n);
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
    EXPECT_EQ(raw::pieceAxis({1000, 1, 1}), 0);
    EXPECT_EQ(raw::pieceAxis({3, 517, 1}), 1);
    EXPECT_EQ(raw::pieceAxis({7, 5, 33}), 2);
}

// An 11.5 ms dispatch in twelve; never more pieces than groups on the axis, the command buffer's bands or kMaxBands
TEST(DispatchPieces, TheCountFollowsTheBudget)
{
    EXPECT_EQ(raw::pieceCount(900'000, TGM_APP_BAND_NS, 100, raw::kBandsPerCommandBuffer), 1u);
    EXPECT_EQ(raw::pieceCount(11'500'000, TGM_APP_BAND_NS, 100, raw::kBandsPerCommandBuffer), 12u);
    EXPECT_EQ(raw::pieceCount(11'500'000, TGM_APP_BAND_NS, 5, raw::kBandsPerCommandBuffer), 5u);
    EXPECT_EQ(raw::pieceCount(11'500'000, TGM_APP_BAND_NS, 1, raw::kBandsPerCommandBuffer), 1u);
    EXPECT_EQ(raw::pieceCount(11'500'000, TGM_APP_BAND_NS, 100, 3), 3u);
    EXPECT_EQ(raw::pieceCount(500'000'000, TGM_APP_BAND_NS, 10'000, raw::kBandsPerCommandBuffer), raw::kMaxBands);
    EXPECT_EQ(raw::pieceCount(11'500'000, 0, 100, raw::kBandsPerCommandBuffer), 1u);
}

// A kernel priced by its instructions until a dispatch of it is measured, then by its time a thread
TEST(DispatchPieces, AMeasuredKernelIsCutByItsTime)
{
    raw::BandRate rate;
    const uint64_t threads = 1024 * 1024;
    const uint64_t staticNs = raw::workNs(raw::laneWork(threads, 40));
    EXPECT_EQ(raw::pieceCount(raw::estimatedNs(rate, threads, staticNs), TGM_APP_BAND_NS, 4096, 64), 1u);
    raw::learn(rate, 6'000'000, threads);
    EXPECT_EQ(raw::pieceCount(raw::estimatedNs(rate, threads, staticNs), TGM_APP_BAND_NS, 4096, 64), 6u);
}

// A launch writes the piece's base and the whole grid's count into the root table's first words, where the kernel's
// compile reads its workgroup base (m2v_source_test holds NVK's compile to reading it there)
TEST(DispatchPieces, ThePiecesBaseIsTheKernelsWorkgroupBase)
{
    const std::string compute = SourceText("../raw/runtime/raw_compute.cpp");
    const std::string launch = Body(compute, "bool ComputeEncoder::launch(");
    EXPECT_THAT(launch, HasSubstr("rootWords(r, piece.base.data(), grid.data(),"));
    EXPECT_THAT(launch, HasSubstr("qmdWords(*state_, piece.groups.data(),"));
    const std::string words = Body(compute, "void ComputeEncoder::rootWords(");
    EXPECT_THAT(words, HasSubstr("root[a] = base[a];"));
    EXPECT_THAT(words, HasSubstr("root[3 + a] = groups[a];"));
    EXPECT_THAT(Body(compute, "bool ComputeEncoder::qmdWords("), HasSubstr("qmd[kQmdRasterWidth] = groups[0];"));
}

}  // namespace
