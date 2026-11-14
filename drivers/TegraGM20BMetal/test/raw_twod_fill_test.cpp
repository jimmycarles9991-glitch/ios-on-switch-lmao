// SPDX-License-Identifier: MIT
// Rule clear.twod's colour words: Metal's codes packed as the UNORM view's solid colour, sRGB channels encoded.
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_twod_fill.h"

namespace {

TEST(TwodFill, WordsInTheUnormViewsPacking)
{
    const float rgba[4] = {1.0f, 0.5f, 0.0f, 0.25f};
    EXPECT_EQ(raw::twodFillWord(80, rgba), 0x40ff8000u);  // A8R8G8B8: alpha, red, green, blue
    EXPECT_EQ(raw::twodFillWord(70, rgba), 0x400080ffu);  // A8B8G8R8: alpha, blue, green, red
    EXPECT_EQ(raw::twodFillWord(81, rgba), 0x40ffbc00u);  // green 0.5 encodes to 188
    const float out[4] = {2.0f, -1.0f, 0.0f, 1.0f};
    EXPECT_EQ(raw::twodFillWord(80, out), 0xffff0000u);
}

TEST(TwodFill, OwnFormatWordsOfTheOtherExactFormats)
{
    const float rgba[4] = {0.2f, 0.4f, 0.6f, 0.8f};
    EXPECT_EQ(raw::twodFillWord(10, rgba), 51u);                                        // Y8: red 0.2
    EXPECT_EQ(raw::twodFillWord(20, rgba), 13107u);                                     // Y16: red 0.2
    EXPECT_EQ(raw::twodFillWord(90, rgba), 2u << 30 | 614u << 20 | 409u << 10 | 205u);  // A2B10G10R10
    const float ends[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    EXPECT_EQ(raw::twodFillWord(90, ends), 0xfff003ffu);
}

TEST(TwodFill, TheChipsExactFormatsHoldOnPlainMemoryOnly)
{
    const struct {
        uint32_t format, fmt2d;
    } exact[] = {{70, 0xd5}, {71, 0xd5}, {80, 0xcf}, {81, 0xcf}, {90, 0xd1}, {10, 0xf3}, {20, 0xee}};

    for (const auto &e : exact) {
        EXPECT_EQ(raw::twodFillFormat(e.format), e.fmt2d) << e.format;
        EXPECT_TRUE(raw::twodFillHolds(e.format, false)) << e.format;
        EXPECT_FALSE(raw::twodFillHolds(e.format, true)) << e.format;
    }
    for (uint32_t f : {115u, 125u, 25u, 55u, 92u, 11u, 30u, 1u})  // floats never; no exact cell for the others
        EXPECT_FALSE(raw::twodFillHolds(f, false)) << f;
}

}  // namespace
