// SPDX-License-Identifier: GPL-2.0-or-later OR LGPL-3.0-or-later
// raw::ArgumentEncoder (raw_argument_encoder.h), the facade's MTLArgumentEncoder: a function's encoder over the runs the bridge
// gives (m2v bind/argument_runs.rs, as HW Test's argument_nested_fragment lays out: a pointer at 0 to a buffer of a texture
// and a colour pointer), its nested encoder's writes, and the refusals Metal's validation stops.
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "../raw/runtime/raw_argument_encoder.h"

namespace {

using raw::ArgumentKind;
using raw::ArgumentRefusal;

constexpr uint32_t kBuffer = uint32_t(ArgumentKind::Buffer), kTexture = uint32_t(ArgumentKind::Texture);

// layout 0: [[id(0)]] a pointer to layout 1 at 0, [[id(1)]] a texture array of 3 at 8; layout 1: [[id(0)]] a texture at 0,
// [[id(1)]] a pointer at 8
const raw::ArgumentRun kOuter[] = {
    {0, 1, 1, 1, kBuffer, 2, 0, 8, 0, 8},
    {1, 3, 1, 1, kTexture, 0, 8, 8, 0, 8},
};
const raw::ArgumentRun kInner[] = {
    {0, 1, 1, 1, kTexture, 0, 0, 8, 0, 8},
    {1, 1, 1, 1, kBuffer, 0, 8, 8, 0, 8},
};

raw::ArgumentEncoder function()
{
    const raw::ArgumentRuns layouts[] = {{32, 8, kOuter, 2}, {16, 8, kInner, 2}};
    raw::ArgumentEncoder e;
    EXPECT_TRUE(raw::ArgumentEncoder::fromRuns(layouts, 2, e));
    return e;
}

uint64_t word(const std::vector<uint8_t> &bytes, size_t at)
{
    uint64_t w;
    memcpy(&w, bytes.data() + at, sizeof w);
    return w;
}

TEST(ArgumentEncoder, AFunctionsEncoderWritesEachArgumentAtItsRunsOffset)
{
    raw::ArgumentEncoder e = function();
    EXPECT_EQ(e.layout().encodedLength, 32u);
    std::vector<uint8_t> bytes(64, 0);
    ASSERT_EQ(e.setBuffer(bytes.data(), bytes.size(), 16, 0), ArgumentRefusal::None);
    EXPECT_EQ(e.writeBuffer(0x100000, 0x1000, 0x40, 0), ArgumentRefusal::None);
    EXPECT_EQ(e.write(1ull << 32 | 27, 3, ArgumentKind::Texture), ArgumentRefusal::None);
    EXPECT_EQ(word(bytes, 16), 0x100040u);
    EXPECT_EQ(word(bytes, 16 + 8 + 2 * 8), 1ull << 32 | 27);
}

TEST(ArgumentEncoder, ANestedEncoderIsThePointedBuffersLayout)
{
    raw::ArgumentEncoder e = function(), inner;
    ASSERT_TRUE(e.nested(0, inner));
    EXPECT_EQ(inner.layout().encodedLength, 16u);
    EXPECT_FALSE(e.nested(1, inner));  // a texture
    EXPECT_FALSE(e.nested(9, inner));  // no argument
    ASSERT_TRUE(e.nested(0, inner));
    std::vector<uint8_t> bytes(16, 0);
    ASSERT_EQ(inner.setBuffer(bytes.data(), bytes.size(), 0, 0), ArgumentRefusal::None);
    EXPECT_EQ(inner.write(1ull << 32 | 5000, 0, ArgumentKind::Texture), ArgumentRefusal::None);
    EXPECT_EQ(inner.writeBuffer(0x7000000, 16, 0, 1), ArgumentRefusal::None);
    EXPECT_EQ(word(bytes, 0), 1ull << 32 | 5000);
    EXPECT_EQ(word(bytes, 8), 0x7000000u);
    raw::ArgumentEncoder again;
    EXPECT_FALSE(inner.nested(1, again));  // the inner pointer names no layout
}

TEST(ArgumentEncoder, WhatMetalsValidationStopsWritesNothing)
{
    raw::ArgumentEncoder e = function();
    std::vector<uint8_t> bytes(40, 0xaa);
    EXPECT_EQ(e.write(1, 1, ArgumentKind::Texture), ArgumentRefusal::NoBuffer);
    EXPECT_EQ(e.setBuffer(nullptr, 64, 0, 0), ArgumentRefusal::NotWritable);
    EXPECT_EQ(e.setBuffer(bytes.data(), bytes.size(), 16, 0), ArgumentRefusal::PastTheEnd);
    EXPECT_EQ(e.setBuffer(bytes.data(), bytes.size(), 0, 1), ArgumentRefusal::PastTheEnd);
    ASSERT_EQ(e.setBuffer(bytes.data(), bytes.size(), 0, 0), ArgumentRefusal::None);
    EXPECT_EQ(e.write(1, 0, ArgumentKind::Texture), ArgumentRefusal::OtherType);
    EXPECT_EQ(e.writeBuffer(0x1000, 16, 16, 0), ArgumentRefusal::OffsetPastEnd);
    EXPECT_EQ(e.fits(0, 2, ArgumentKind::Texture), ArgumentRefusal::OtherType);  // 0 is a pointer
    EXPECT_EQ(e.fits(1, 3, ArgumentKind::Texture), ArgumentRefusal::None);
    for (uint8_t b : bytes)
        EXPECT_EQ(b, 0xaa);
}

// Plain Metal: a setter at an index no argument has writes at the arguments' start while 8 bytes fit there
TEST(ArgumentEncoder, AnIndexNoArgumentHasWritesAtTheStart)
{
    raw::ArgumentEncoder e = function();
    std::vector<uint8_t> bytes(32, 0);
    ASSERT_EQ(e.setBuffer(bytes.data(), bytes.size(), 0, 0), ArgumentRefusal::None);
    EXPECT_EQ(e.write(0x55, 40, ArgumentKind::Sampler), ArgumentRefusal::None);
    EXPECT_EQ(word(bytes, 0), 0x55u);
    ArgumentRefusal why;
    EXPECT_EQ(e.constantData(40, why), bytes.data());
    EXPECT_EQ(e.constantData(1, why), bytes.data() + 8);
}

TEST(ArgumentEncoder, AFunctionOfNoLayoutsOrABadRunHasNoEncoder)
{
    raw::ArgumentEncoder e;
    EXPECT_FALSE(raw::ArgumentEncoder::fromRuns(nullptr, 0, e));
    const raw::ArgumentRun bad[] = {{0, 1, 1, 1, 99, 0, 0, 8, 0, 8}};
    const raw::ArgumentRuns layouts[] = {{8, 8, bad, 1}};
    EXPECT_FALSE(raw::ArgumentEncoder::fromRuns(layouts, 1, e));
    const raw::ArgumentRun past[] = {{0, 1, 1, 1, kBuffer, 3, 0, 8, 0, 8}};
    const raw::ArgumentRuns pastLayouts[] = {{8, 8, past, 1}};
    EXPECT_FALSE(raw::ArgumentEncoder::fromRuns(pastLayouts, 1, e));
}

}  // namespace
