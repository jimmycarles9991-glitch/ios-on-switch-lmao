// SPDX-License-Identifier: MIT
// Rule clear.twod: the formats whose fills the 2D engine writes, on plain memory only.
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/twod_fill.h"

namespace {

TEST(TwodFill, TheChipsExactFormatsHoldOnPlainMemoryOnly)
{
    const struct {
        uint32_t format, fmt2d;
    } exact[] = {{70, 0xd5}, {71, 0xd5}, {80, 0xcf}, {81, 0xcf}, {90, 0xd1}, {10, 0xf3}, {20, 0xee}};

    for (const auto &e : exact) {
        EXPECT_EQ(gm20b::twodFillFormat(e.format), e.fmt2d) << e.format;
        EXPECT_TRUE(gm20b::twodFillHolds(e.format, false)) << e.format;
        EXPECT_FALSE(gm20b::twodFillHolds(e.format, true)) << e.format;
    }
    for (uint32_t f : {115u, 125u, 25u, 55u, 92u, 11u, 30u, 1u})  // floats never; no exact cell for the others
        EXPECT_FALSE(gm20b::twodFillHolds(f, false)) << f;
}

}  // namespace
