// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_nonfinite.h"

TEST(RawNonFinite, OnlyFloatFormatsCanHoldThem)
{
    EXPECT_EQ(raw::floatComponentBytes(115), 2u);  // RGBA16Float
    EXPECT_EQ(raw::floatComponentBytes(55), 4u);   // R32Float
    EXPECT_EQ(raw::floatComponentBytes(80), 0u);   // BGRA8Unorm: stale bytes are only wrong colours
    EXPECT_EQ(raw::floatComponentBytes(554), 0u);  // BGR10_XR: fixed point
}

// A BGRA8 picture's white texels read as RGBA16Float are NaN halves; its black ones are zero.
TEST(RawNonFinite, StaleWhiteBgraIsNaNAsHalves)
{
    std::vector<uint8_t> memory(4096, 0);
    for (size_t i = 1024; i < 1088; ++i)
        memory[i] = 0xff;
    uint32_t lines = 0;
    const uint32_t found = raw::sampledNonFinite(memory.data(), memory.size(), 2, 1024, [&](const uint8_t *) {
        ++lines;
    });
    EXPECT_EQ(lines, 4u);
    EXPECT_EQ(found, 32u);
    EXPECT_EQ(raw::sampledNonFinite(memory.data(), memory.size(), 2, 2048, [](const uint8_t *) {}), 0u);
}

TEST(RawNonFinite, InfinityCountsAndFiniteExtremesDoNot)
{
    const uint16_t halves[32] = {0x7c00, 0xfc00, 0x7bff, 0xfbff, 0x3c00};
    EXPECT_EQ(
        raw::sampledNonFinite(reinterpret_cast<const uint8_t *>(halves), sizeof halves, 2, 64, [](const uint8_t *) {}),
        2u);
    const uint32_t floats[16] = {0x7f800000, 0x7fc00000, 0x7f7fffff, 0x3f800000};
    EXPECT_EQ(
        raw::sampledNonFinite(reinterpret_cast<const uint8_t *>(floats), sizeof floats, 4, 64, [](const uint8_t *) {}),
        2u);
}

// Glass uniforms: a NaN float is counted as a word at its offset; a NaN half inside a finite float's low half only as a half
TEST(RawNonFinite, UniformWordsAndHalvesAreCountedApart)
{
    uint32_t words[8] = {0x3f800000, 0x00007e00, 0x7fc00000, 0x40000000};
    const auto found = raw::nonFiniteUniforms(reinterpret_cast<const uint8_t *>(words), sizeof words);
    EXPECT_EQ(found.words, 1u);
    EXPECT_EQ(found.first, 8u);
    EXPECT_EQ(found.halves, 2u);  // the low half of word 1, and the high half of word 2 (0x7fc0)
    uint16_t halves[4] = {0x3c00, 0x7c00};
    const auto half = raw::nonFiniteUniforms(reinterpret_cast<const uint8_t *>(halves), sizeof halves);
    EXPECT_EQ(half.words, 0u);
    EXPECT_EQ(half.halves, 1u);
    EXPECT_EQ(half.first, 2u);
    EXPECT_EQ(raw::nonFiniteUniforms(reinterpret_cast<const uint8_t *>(halves), 1).first, UINT64_MAX);
}
