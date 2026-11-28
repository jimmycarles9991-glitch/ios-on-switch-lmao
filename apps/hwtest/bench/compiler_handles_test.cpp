// SPDX-License-Identifier: MIT
#include "apps/hwtest/bench/compiler_handles.h"
#include <array>
#include <gtest/gtest.h>

TEST(CompilerHandles, ImagesAndBothSampledKindsCanInterleave)
{
    std::array<uint32_t, 4> table{};
    bench::compiler::HandleWriter handles(table.data(), {3, 1, 6, 3, 2, 7}, false);
    handles.image(31);
    handles.sampled(11);
    handles.image(32);
    handles.sampled(22);
    EXPECT_EQ(table, (std::array<uint32_t, 4>{11, 22, 31, 32}));
}

TEST(CompilerHandles, RenderBoxKeepsItsVertexStorageGap)
{
    std::array<uint32_t, 17> table{};
    bench::compiler::HandleWriter handles(table.data(), {3, 2}, true);
    handles.image(31);
    handles.sampled(22);
    EXPECT_EQ(table[0], 22u);
    EXPECT_EQ(table[1], 0u);
    EXPECT_EQ(table[16], 31u);
}
