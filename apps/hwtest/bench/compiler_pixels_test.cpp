// SPDX-License-Identifier: MIT
#include "apps/hwtest/bench/compiler_pixels.h"
#include <gtest/gtest.h>

TEST(CompilerPixels, ColorSamplesOfEachPixelAreConsecutive)
{
    const bench::compiler::Attachment color{1, false, 3, 2, 4};
    std::vector<uint8_t> warmup(3 * 2 * 4 * 4);
    for (size_t sample = 0; sample < 4; ++sample) {
        auto last = warmup;
        last[((3 + 1) * 4 + sample) * 4 + 2] = 0xab;
        EXPECT_EQ(bench::compiler::difference(color, warmup, last, "draw 0", "draw 100"),
                  "color 1 (1,1) sample " + std::to_string(sample) + " draw 0 00000000 draw 100 0000ab00");
    }
}

TEST(CompilerPixels, DepthSamplesFollowThePhysicalTwoByTwoGrid)
{
    const bench::compiler::Attachment zeta{1, true, 3, 2, 4};
    std::vector<uint8_t> warmup(3 * 2 * 4 * 8);
    EXPECT_TRUE(bench::compiler::difference(zeta, warmup, warmup, "a", "b").empty());
    auto last = warmup;
    last[(3 * 6 + 2) * 8 + 4] = 0xff;
    EXPECT_EQ(bench::compiler::difference(zeta, warmup, last, "a", "b"),
              "depth/stencil (1,1) sample 2 a 0000000000000000 b 00000000ff000000");
}
