// SPDX-License-Identifier: GPL-2.0-or-later OR LGPL-3.0-or-later
// Every expected offset, length and alignment is what Metal's encoder answered on an M3 Max and an M4 (metal2vulkan's
// tests/argument_buffers.rs, probes d1 to d18).
#include <gtest/gtest.h>
#include <utility>
#include "../raw/runtime/raw_argument_layout.h"

namespace {
using raw::ArgumentDescription;
using raw::ArgumentKind;
using raw::ArgumentLayout;
using raw::ArgumentLayoutError;
using raw::ArgumentType;
using K = ArgumentKind;

ArgumentType handle(K kind)
{
    return {kind, 0, 1, 1};
}

ArgumentType constant(uint8_t scalar, uint8_t rows = 1, uint8_t columns = 1)
{
    return {K::Constant, scalar, columns, rows};
}

const ArgumentType Pointer = handle(K::Buffer), Texture = handle(K::Texture), Sampler = handle(K::Sampler);
const ArgumentType Float = constant(4), Float3 = constant(4, 3), Float4 = constant(4, 4), Char = constant(1),
                   Half = constant(2), Int = constant(4);

ArgumentDescription one(ArgumentType type, uint32_t index)
{
    return {type, index, 0, 0};
}

ArgumentDescription array(ArgumentType type, uint32_t index, uint32_t length)
{
    return {type, index, length, 0};
}

ArgumentDescription aligned(ArgumentType type, uint32_t index, uint32_t alignment)
{
    return {type, index, 0, alignment};
}

ArgumentLayout layout(const std::vector<ArgumentDescription> &descriptions)
{
    ArgumentLayout l;
    size_t position = 0;
    EXPECT_EQ(ArgumentLayout::fromDescriptions(descriptions, l, position), ArgumentLayoutError::None);
    return l;
}

uint64_t at(const ArgumentLayout &l, uint32_t index)
{
    raw::ArgumentPlace place;
    EXPECT_TRUE(l.place(index, place)) << "argument " << index;
    return place.offset;
}

std::pair<uint64_t, uint64_t> sized(const ArgumentLayout &l)
{
    return {l.encodedLength, l.alignment};
}

TEST(ArgumentLayout, DescriptorsLayOutAsACStructInTheOrderGiven)
{
    const auto l = layout({one(Pointer, 0), one(Texture, 1), one(Sampler, 2), one(Float, 3), one(Float4, 4)});
    EXPECT_EQ(sized(l), std::make_pair(uint64_t(48), uint64_t(16)));
    raw::ArgumentPlace place;
    ASSERT_TRUE(l.place(1, place));
    EXPECT_EQ(place.kind, K::Texture);
    EXPECT_EQ((std::vector<uint64_t>{at(l, 0), at(l, 1), at(l, 2), at(l, 3), at(l, 4)}),
              (std::vector<uint64_t>{0, 8, 16, 24, 32}));
    const auto gaps = layout({one(Texture, 9), one(Float, 2), one(Pointer, 5)});
    EXPECT_EQ(sized(gaps), std::make_pair(uint64_t(24), uint64_t(8)));
    EXPECT_EQ((std::vector<uint64_t>{at(gaps, 9), at(gaps, 2), at(gaps, 5)}), (std::vector<uint64_t>{0, 8, 16}));
    EXPECT_FALSE(gaps.place(3, place));
    const auto objects =
        layout({one(handle(K::IndirectCommandBuffer), 0), one(handle(K::ComputePipeline), 1),
                one(handle(K::RenderPipeline), 2), one(handle(K::VisibleFunctionTable), 3), one(Char, 4)});
    EXPECT_EQ(sized(objects), std::make_pair(uint64_t(40), uint64_t(8)));
    EXPECT_EQ(at(objects, 4), 32u);
}

TEST(ArgumentLayout, ArraysTakeAnIndexPerElement)
{
    const auto l = layout({array(Texture, 0, 3), array(Pointer, 3, 2), array(Float, 5, 4), array(Sampler, 9, 2)});
    EXPECT_EQ(sized(l), std::make_pair(uint64_t(72), uint64_t(8)));
    std::vector<uint64_t> offsets;
    for (uint32_t i = 0; i < 11; i++)
        offsets.push_back(at(l, i));
    EXPECT_EQ(offsets, (std::vector<uint64_t>{0, 8, 16, 24, 32, 40, 44, 48, 52, 56, 64}));
    raw::ArgumentPlace place;
    EXPECT_FALSE(l.place(11, place));
    const auto packed = layout({array(constant(1), 0, 3), one(Float, 3)});
    EXPECT_EQ(sized(packed), std::make_pair(uint64_t(8), uint64_t(4)));
    EXPECT_EQ((std::vector<uint64_t>{at(packed, 1), at(packed, 2), at(packed, 3)}), (std::vector<uint64_t>{1, 2, 4}));
}

TEST(ArgumentLayout, ConstantsTakeMslSizesAndAlignments)
{
    // [texture, type, char]: the type's offset, the char's, length, alignment
    const struct {
        ArgumentType type;
        uint64_t offset, next, length, alignment;
    } cases[] = {
        {constant(2, 3), 8, 16, 24, 8},       // bfloat3
        {constant(8, 3), 32, 64, 96, 32},     // long3
        {constant(8, 4), 32, 64, 96, 32},     // ulong4
        {constant(2, 3, 2), 8, 24, 32, 8},    // half2x3
        {constant(2, 3, 3), 8, 32, 40, 8},    // half3x3
        {constant(2, 3), 8, 16, 24, 8},       // short3
        {constant(1, 3), 8, 12, 16, 8},       // char3
        {constant(4, 2, 3), 8, 32, 40, 8},    // float3x2
        {constant(4, 3, 4), 16, 80, 96, 16},  // float4x3
        {constant(4, 3), 16, 32, 48, 16},     // int3
        {handle(K::DepthStencilState), 8, 16, 24, 8},
    };

    for (const auto &c : cases) {
        const auto l = layout({one(Texture, 0), one(c.type, 1), one(Char, 2)});
        EXPECT_EQ(at(l, 1), c.offset);
        EXPECT_EQ(at(l, 2), c.next);
        EXPECT_EQ(sized(l), std::make_pair(c.length, c.alignment));
    }
    const auto mixed = layout({one(Char, 0), one(Float4, 1), one(Char, 2), one(Float3, 3), one(Half, 4)});
    EXPECT_EQ(sized(mixed), std::make_pair(uint64_t(80), uint64_t(16)));
    EXPECT_EQ(at(mixed, 4), 64u);
    const auto matrices =
        layout({one(constant(4, 4, 4), 0), one(constant(4, 2), 1), one(constant(1), 2), one(constant(2, 3), 3)});
    EXPECT_EQ(sized(matrices), std::make_pair(uint64_t(96), uint64_t(16)));
    EXPECT_EQ((std::vector<uint64_t>{at(matrices, 1), at(matrices, 2), at(matrices, 3)}),
              (std::vector<uint64_t>{64, 72, 80}));
    EXPECT_EQ(sized(layout({one(Char, 0)})), std::make_pair(uint64_t(1), uint64_t(1)));
}

TEST(ArgumentLayout, AConstantBlockAlignmentMovesOnlyItsBlock)
{
    const struct {
        std::vector<ArgumentDescription> descriptions;
        uint32_t index;
        uint64_t offset, length, alignment;
    } cases[] = {
        {{aligned(Char, 0, 64), one(Float, 1)}, 1, 4, 8, 4},
        {{one(Pointer, 0), aligned(Char, 1, 64), one(Float, 2)}, 2, 68, 72, 8},
        {{one(Texture, 0), aligned(Int, 1, 32), one(Int, 2)}, 1, 32, 40, 8},
        {{one(Texture, 0), aligned(Char, 1, 3)}, 1, 8, 16, 8},
        {{aligned(Char, 0, 1024)}, 0, 0, 1, 1},
        {{aligned(Float4, 0, 4), one(Char, 1)}, 1, 16, 32, 16},
        {{one(Texture, 0), aligned(Char, 1, 12)}, 1, 16, 24, 8},
        {{one(Texture, 0), aligned(Char, 1, 20)}, 1, 8, 16, 8},
        {{one(Texture, 0), aligned(Char, 1, 48)}, 1, 16, 24, 8},
        {{one(Texture, 0), aligned(Float4, 1, 40)}, 1, 8, 32, 16},
        {{one(Texture, 0), aligned(Float, 1, 6)}, 1, 8, 16, 8},
    };

    for (const auto &c : cases) {
        const auto l = layout(c.descriptions);
        EXPECT_EQ(at(l, c.index), c.offset);
        EXPECT_EQ(sized(l), std::make_pair(c.length, c.alignment));
    }
    const auto blocks = layout({one(Pointer, 0), one(Char, 1), one(Pointer, 2), aligned(Char, 3, 32), one(Char, 4)});
    EXPECT_EQ(sized(blocks), std::make_pair(uint64_t(40), uint64_t(8)));
    EXPECT_EQ((std::vector<uint64_t>{at(blocks, 3), at(blocks, 4)}), (std::vector<uint64_t>{32, 33}));
    ArgumentDescription chars = array(Char, 1, 3);
    chars.constantBlockAlignment = 16;
    const auto arrayBlock = layout({one(Texture, 0), chars, one(Float, 4)});
    EXPECT_EQ((std::vector<uint64_t>{at(arrayBlock, 1), at(arrayBlock, 2), at(arrayBlock, 3), at(arrayBlock, 4)}),
              (std::vector<uint64_t>{16, 17, 18, 20}));
    EXPECT_EQ(sized(arrayBlock), std::make_pair(uint64_t(24), uint64_t(8)));
}

TEST(ArgumentLayout, WhatMetalRefusesIsRefusedAndALaterClaimWins)
{
    ArgumentLayout l;
    size_t position = 0;
    EXPECT_EQ(ArgumentLayout::fromDescriptions({}, l, position), ArgumentLayoutError::NoArguments);
    EXPECT_EQ(ArgumentLayout::fromDescriptions({one(Char, 0), aligned(Float4, 1, 4)}, l, position),
              ArgumentLayoutError::ConstantBlockAlignment);
    EXPECT_EQ(position, 1u);
    const auto twice = layout({one(Float, 0), one(Pointer, 0)});
    EXPECT_EQ(sized(twice), std::make_pair(uint64_t(16), uint64_t(8)));
    EXPECT_EQ(at(twice, 0), 8u);
    const auto inside = layout({array(Texture, 0, 3), one(Pointer, 1)});
    EXPECT_EQ((std::vector<uint64_t>{at(inside, 0), at(inside, 1), at(inside, 2)}), (std::vector<uint64_t>{0, 24, 16}));
}

// m2v's bind/argument_runs.rs runs of a struct array whose elements hold a texture array: `struct S { sampler s [[id(0)]];
// texture2d t[2] [[id(1)]]; } arr[3] [[id(10)]]` (24 bytes each, from 16), a pointer to an argument buffer at 40, and a later
// constant at index 12 that wins over the second element's sampler
TEST(ArgumentLayout, AFunctionsRunsPlaceStructArraysOfArraysAndNestedBuffers)
{
    const raw::ArgumentRun runs[] = {
        {10, 3, 3, 1, uint32_t(K::Sampler), 0, 16, 24, 0, 8},
        {11, 3, 3, 2, uint32_t(K::Texture), 0, 24, 24, 8, 8},
        {40, 1, 1, 1, uint32_t(K::Buffer), 2, 88, 8, 0, 8},
        {13, 1, 1, 1, uint32_t(K::Constant), 0, 96, 4, 0, 4},
    };
    ArgumentLayout l;
    ASSERT_TRUE(ArgumentLayout::fromRuns(104, 8, runs, 4, 2, l));
    EXPECT_EQ(sized(l), std::make_pair(uint64_t(104), uint64_t(8)));
    EXPECT_EQ((std::vector<uint64_t>{at(l, 10), at(l, 11), at(l, 12), at(l, 13), at(l, 14), at(l, 16), at(l, 17)}),
              (std::vector<uint64_t>{16, 24, 32, 96, 48, 64, 72}));
    raw::ArgumentPlace place;
    for (uint32_t gap : {9u, 19u, 20u, 39u, 41u})
        EXPECT_FALSE(l.place(gap, place)) << gap;
    ASSERT_TRUE(l.place(40, place));
    EXPECT_EQ(place.kind, K::Buffer);
    EXPECT_EQ(place.nested, 1);
    ASSERT_TRUE(l.place(13, place));
    EXPECT_EQ(place.kind, K::Constant);
    EXPECT_EQ(place.nested, -1);
    const raw::ArgumentRun past = {0, 1, 1, 1, uint32_t(K::Buffer), 3, 0, 8, 0, 8};
    EXPECT_FALSE(ArgumentLayout::fromRuns(8, 8, &past, 1, 2, l));
}

}  // namespace
