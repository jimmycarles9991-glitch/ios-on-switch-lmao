// SPDX-License-Identifier: MIT
// raw/runtime/raw_uniform_tuples.h, the DEVELOPMENT line that counts the uniform value tuples Core Animation's generic merge
// states are drawn with: a tuple is the bytes the record's uniform ranges read (boot 21's line hashed 1024 bytes of each
// bound arena, so every draw was a new tuple and its 64-tuple cap was full within a second), each 5 s window's draws per
// tuple are counted in bounded memory, and only a generic merge state's draw is counted, in DEVELOPMENT builds alone.
#include "../raw/runtime/raw_uniform_tuples.h"
#include "bazel/testing/source_text.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace {

// Two uniform buffers of the vertex stage: slot 0 over a 256-byte window (Core Animation's generic states' entries), slot 2
// the whole buffer; and one of the fragment stage at slot 1 whose record bounds the read to [32, 64)
raw::PackPipeline record()
{
    raw::PackPipeline r = {};
    r.entries = 3;
    r.entry[0] = {0, raw::EntryUbo, 1, 0, 4, 16, 0};
    r.entry[1] = {16, raw::EntryUbo, 1, 2, 12, 0, 0};
    r.entry[2] = {32, raw::EntryUbo, 2, 1, 68, 0, 1};
    memset(r.uniform, raw::kNoUniform, sizeof r.uniform);
    r.uniform[0] = {0, 3, 0, 0};
    r.uniform[1] = {1, 1, 0, 0};
    r.uniform[2] = {2, 5, 2, 4};
    r.keyHash = 0xb0f4f3daabb01e2eull;
    return r;
}

struct Buffers {
    std::array<uint8_t, 4096> arena{}, whole{}, fragment{};
    bool mapped = true;

    raw::BoundBytes operator()(uint8_t stage, uint8_t slot) const
    {
        if (!mapped)
            return {nullptr, arena.size()};
        if (stage == 2)
            return {fragment.data(), fragment.size()};
        return slot == 0 ? raw::BoundBytes{arena.data(), arena.size()} : raw::BoundBytes{whole.data(), 40};
    }
};

uint64_t tupleOf(const Buffers &b, bool &mapped)
{
    uint64_t tuple = 0;
    mapped = raw::uniformTuple(record(), b, tuple);
    return tuple;
}

TEST(RawUniformTuples, ATupleIsTheBytesTheRecordsRangesRead)
{
    const raw::PackPipeline r = record();
    EXPECT_EQ(raw::uniformRead(r, r.uniform[0], 4096).end, 256u);
    EXPECT_EQ(raw::uniformRead(r, r.uniform[0], 100).end, 100u);
    EXPECT_EQ(raw::uniformRead(r, r.uniform[1], 40).end, 40u);
    EXPECT_EQ(raw::uniformRead(r, r.uniform[2], 4096).first, 32u);
    EXPECT_EQ(raw::uniformRead(r, r.uniform[2], 4096).end, 64u);
    EXPECT_EQ(raw::uniformRead(r, r.uniform[2], 16).first, 16u);

    Buffers b;
    bool mapped = false;
    const uint64_t base = tupleOf(b, mapped);
    ASSERT_TRUE(mapped);
    Buffers tails = b;
    tails.arena[256] = tails.arena[4095] = 7;
    tails.whole[40] = 9;
    tails.fragment[31] = tails.fragment[64] = 3;
    EXPECT_EQ(tupleOf(tails, mapped), base);
    const auto differs = [&](void (*change)(Buffers &)) {
        Buffers c = b;
        change(c);
        bool m = false;
        return tupleOf(c, m) != base;
    };
    EXPECT_TRUE(differs([](Buffers &c) {
        c.arena[255] ^= 1;
    }));
    EXPECT_TRUE(differs([](Buffers &c) {
        c.whole[39] ^= 1;
    }));
    EXPECT_TRUE(differs([](Buffers &c) {
        c.fragment[32] ^= 1;
    }));
    EXPECT_TRUE(differs([](Buffers &c) {
        c.fragment[63] ^= 1;
    }));
    Buffers unmapped = b;
    unmapped.mapped = false;
    tupleOf(unmapped, mapped);
    EXPECT_FALSE(mapped);
}

TEST(RawUniformTuples, EachWindowCountsItsTuplesAndTheirDraws)
{
    raw::UniformTuples tuples;
    std::vector<raw::UniformTuples::Window> closed;
    const uint64_t a = 0xb0f4f3daabb01e2eull, b = 0x64ed02fd5d7a1865ull;
    for (uint64_t i = 0; i < 150; i++)
        tuples.note(a, true, 1, 1000 + i, closed);
    for (uint64_t i = 0; i < 20; i++)
        tuples.note(a, true, 2 + i % 2, 2000 + i, closed);
    tuples.note(a, true, 4, 3000, closed);
    tuples.note(a, false, 0, 3001, closed);
    tuples.note(b, true, 1, 4000, closed);
    EXPECT_TRUE(closed.empty());
    tuples.note(b, true, 1, 1000 + raw::UniformTuples::kWindowUs, closed);
    ASSERT_EQ(closed.size(), 1u);
    const raw::UniformTuples::Window &w = closed[0];
    EXPECT_EQ(w.keyHash, a);
    EXPECT_EQ(w.draws, 172u);
    EXPECT_EQ(w.tuples, 4u);
    EXPECT_EQ(w.drawnOnce, 1u);
    EXPECT_EQ(w.drawnUnder10, 0u);
    EXPECT_EQ(w.drawnUnder100, 2u);
    EXPECT_EQ(w.drawnMore, 1u);
    EXPECT_EQ(w.most, 150u);
    EXPECT_EQ(w.mostTuple, 1u);
    EXPECT_EQ(w.unmapped, 1u);
    closed.clear();
    tuples.note(a, true, 1, 2000 + raw::UniformTuples::kWindowUs, closed);
    EXPECT_TRUE(closed.empty());
    tuples.note(a, true, 1, 4000 + raw::UniformTuples::kWindowUs, closed);
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_EQ(closed[0].keyHash, b);
    EXPECT_EQ(closed[0].draws, 2u);
}

// Thousands of distinct tuples a second (boot 21's arena tails) stay within the bound and are still counted
TEST(RawUniformTuples, ItsMemoryIsBounded)
{
    raw::UniformTuples tuples;
    std::vector<raw::UniformTuples::Window> closed;
    const size_t many = raw::UniformTuples::kTuplesPerState + 500;
    for (uint64_t i = 0; i < many; i++)
        tuples.note(7, true, i, 10 + i, closed);
    for (uint64_t s = 0; s < 2 * raw::UniformTuples::kStates; s++)
        tuples.note(100 + s, true, 0, 10 + many, closed);
    tuples.note(7, true, 0, 10 + many + raw::UniformTuples::kWindowUs, closed);
    ASSERT_EQ(closed.size(), raw::UniformTuples::kStates);
    const auto first = std::find_if(closed.begin(), closed.end(), [](const auto &w) {
        return w.keyHash == 7;
    });
    ASSERT_NE(first, closed.end());
    EXPECT_EQ(first->tuples, raw::UniformTuples::kTuplesPerState);
    EXPECT_EQ(first->beyond, 500u);
    EXPECT_EQ(first->draws, many);
}

// Counted for a generic merge state's draw alone, and in DEVELOPMENT builds alone: a release build carries none of it
TEST(RawUniformTuples, OnlyAGenericMergeStatesDrawIsCountedInADevelopmentBuild)
{
    const std::string gpu = switch_ios::test::SourceText("../raw/runtime/raw_gpu.cpp");
    const size_t prepare = gpu.find("bool RenderEncoder::prepare(Pipeline *pl,");
    ASSERT_NE(prepare, std::string::npos);
    const size_t taken = gpu.find("#if SWITCH_RAW_DIAGNOSTICS\n"
                                  "    const bool genericMerge = pl->genericState;\n"
                                  "#endif",
                                  prepare);
    const size_t logged = gpu.find("#if SWITCH_RAW_DIAGNOSTICS\n"
                                   "    if (genericMerge)\n"
                                   "        logUniformTuple(*pl);\n"
                                   "#endif",
                                   prepare);
    EXPECT_NE(taken, std::string::npos);
    EXPECT_NE(logged, std::string::npos);
    EXPECT_EQ(gpu.find("logUniformTuple(", logged + 60), std::string::npos);
    const std::string variants = switch_ios::test::SourceText("../raw/runtime/raw_variants.cpp");
    const size_t defined = variants.find("void RenderEncoder::logUniformTuple(");
    ASSERT_NE(defined, std::string::npos);
    EXPECT_LT(variants.rfind("#if SWITCH_RAW_DIAGNOSTICS", defined), defined);
    EXPECT_GT(variants.find("#endif", defined), defined);
}

}  // namespace
