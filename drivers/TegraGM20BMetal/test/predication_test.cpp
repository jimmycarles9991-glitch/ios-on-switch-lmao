// SPDX-License-Identifier: MIT
// raw's command predication run on a model of the units it drives (predication_model.h). Each program's launches must be
// the ones Metal runs.
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/predicate_records.h"
#include "drivers/TegraGM20BMetal/predication.h"
#include "drivers/TegraGM20BMetal/test/predication_model.h"
#include "kits/SwitchGM20B/SwitchGM20B.h"

namespace {

using predication_model::Gpu;
using predication_model::kBase;
using predication_model::kCompute;
constexpr uint32_t kEdges[] = {0, 1, 2, 7, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu};

// A command buffer's compute words: the predication and launches named 1, 2, ... in encode order
struct Program {
    Gpu gpu;
    gm20b::Predication predication;
    std::vector<uint32_t> words = std::vector<uint32_t>(1 << 14);
    gm20b::Push p{words.data(), words.data() + words.size()};
    uint64_t nextSlot = kBase + 0x1000;
    uint32_t launches = 0;

    gm20b::PredicateSlot slot()
    {
        const gm20b::PredicateSlot s{gpu.at(nextSlot), nextSlot};
        nextSlot += gm20b::Predication::kSlotBytes;
        return s;
    }

    uint64_t wordVa(uint32_t i) const
    {
        return kBase + 4 * i;
    }

    void startIf(uint32_t word, gm20b::PredicateCompare c, uint32_t reference)
    {
        const uint32_t i = uint32_t(predication.depth());
        gpu.put(wordVa(i), word);
        uint32_t *before = p.at;
        EXPECT_EQ(predication.startIf(p, slot(), wordVa(i), gm20b::predicateTest(c, reference)),
                  gm20b::Predication::Answer::Done);
        EXPECT_LE(uint32_t(p.at - before), gm20b::Predication::kStartWords);
    }

    void startElse()
    {
        uint32_t *before = p.at;
        EXPECT_EQ(predication.startElse(p, slot()), gm20b::Predication::Answer::Done);
        EXPECT_LE(uint32_t(p.at - before), gm20b::Predication::kStartWords);
    }

    void dispatch()
    {
        predication.guard(p);
        p.m(NVB1C0_SEND_PCAS_A, ++launches, kCompute);
        p.m(NVB1C0_SEND_SIGNALING_PCAS_B, 0, kCompute);
        predication.release(p);
    }

    std::vector<uint32_t> run()
    {
        gpu.run(std::vector<uint32_t>(words.data(), p.at));
        return gpu.launched;
    }
};

TEST(GM20BPredication, MetalsCodesAboveFiveCompareEqual)
{
    EXPECT_EQ(gm20b::predicateCompare(5), gm20b::PredicateCompare::LessEqual);
    for (uint64_t code : {6ull, 255ull, (1ull << 32) + 2, ~0ull})
        EXPECT_EQ(gm20b::predicateCompare(code), gm20b::PredicateCompare::Equal);
}

TEST(GM20BPredication, EveryCompareReducesToTheUnitsEquality)
{
    const auto holds = [](gm20b::PredicateCompare c, uint32_t w, uint32_t r) {
        switch (c) {
        case gm20b::PredicateCompare::Equal:
            return w == r;
        case gm20b::PredicateCompare::NotEqual:
            return w != r;
        case gm20b::PredicateCompare::Greater:
            return w > r;
        case gm20b::PredicateCompare::GreaterEqual:
            return w >= r;
        case gm20b::PredicateCompare::Less:
            return w < r;
        case gm20b::PredicateCompare::LessEqual:
            return w <= r;
        }
        return false;
    };
    std::vector<uint32_t> values(std::begin(kEdges), std::end(kEdges));
    for (uint32_t v = 0; v < 64; ++v)
        values.push_back(v);
    for (uint32_t c = 0; c < 6; ++c)
        for (uint32_t r : values)
            for (uint32_t w : values)
                ASSERT_EQ(gm20b::predicateRuns(gm20b::predicateTest(gm20b::PredicateCompare(c), r), w),
                          holds(gm20b::PredicateCompare(c), w, r))
                    << "compare " << c << " word " << w << " reference " << r;
}

TEST(GM20BPredication, OneBlockRunsItsDispatchExactlyWhereTheWordCompares)
{
    for (uint32_t c = 0; c < 6; ++c)
        for (uint32_t r : kEdges)
            for (uint32_t w : kEdges) {
                Program prog;
                prog.dispatch();
                prog.startIf(w, gm20b::PredicateCompare(c), r);
                prog.dispatch();
                prog.startElse();
                prog.dispatch();
                EXPECT_EQ(prog.predication.endIf(), gm20b::Predication::Answer::Done);
                prog.dispatch();
                const bool holds = gm20b::predicateRuns(gm20b::predicateTest(gm20b::PredicateCompare(c), r), w);
                EXPECT_EQ(prog.run(), (std::vector<uint32_t>{1, holds ? 2u : 3u, 4}))
                    << "compare " << c << " word " << w << " reference " << r;
            }
}

TEST(GM20BPredication, NestedBlocksRunWhereEveryEnclosingBranchHolds)
{
    std::mt19937 rng(1234);
    for (int trial = 0; trial < 4000; ++trial) {
        Program prog;
        std::vector<uint32_t> expected;
        std::vector<bool> live{true};
        const int depth = 1 + int(rng() % 4);
        auto dispatchAndExpect = [&] {
            prog.dispatch();
            if (live.back())
                expected.push_back(prog.launches);
        };
        std::vector<bool> conditions;
        for (int d = 0; d < depth; ++d) {
            dispatchAndExpect();
            const auto c = gm20b::PredicateCompare(rng() % 6);
            const uint32_t r = rng() % 3 ? rng() % 8 : kEdges[rng() % 8];
            const uint32_t w = rng() % 3 ? rng() % 8 : kEdges[rng() % 8];
            prog.startIf(w, c, r);
            conditions.push_back(gm20b::predicateRuns(gm20b::predicateTest(c, r), w));
            live.push_back(live.back() && conditions.back());
            dispatchAndExpect();
        }
        for (int d = depth - 1; d >= 0; --d) {
            if (rng() % 2) {
                prog.startElse();
                live.pop_back();
                live.push_back(live.back() && !conditions[size_t(d)]);
                dispatchAndExpect();
            }
            EXPECT_EQ(prog.predication.endIf(), gm20b::Predication::Answer::Done);
            live.pop_back();
            dispatchAndExpect();
        }
        ASSERT_EQ(prog.run(), expected) << "trial " << trial;
        ASSERT_TRUE(prog.gpu.enabled);
    }
}

TEST(GM20BPredication, MisusedRecordsAreRefusedWithoutWords)
{
    Program prog;
    uint32_t *before = prog.p.at;
    EXPECT_EQ(prog.predication.startElse(prog.p, prog.slot()), gm20b::Predication::Answer::Misused);
    EXPECT_EQ(prog.predication.endIf(), gm20b::Predication::Answer::Misused);
    prog.startIf(1, gm20b::PredicateCompare::Equal, 1);
    prog.startElse();
    before = prog.p.at;
    EXPECT_EQ(prog.predication.startElse(prog.p, prog.slot()), gm20b::Predication::Answer::Misused);
    EXPECT_EQ(prog.p.at, before);
}

TEST(GM20BPredication, ADispatchOutsideABlockCarriesNoRenderEnable)
{
    Program prog;
    prog.dispatch();
    EXPECT_EQ(prog.p.at - prog.words.data(), 2);
    prog.startIf(3, gm20b::PredicateCompare::Greater, 2);
    prog.predication.endUnit();
    prog.dispatch();
    EXPECT_EQ(prog.run(), (std::vector<uint32_t>{1, 2}));
}

}  // namespace

namespace {

using Records = gm20b::PredicateRecords;

TEST(GM20BPredicateRecords, OnlyAnEndOfItsOwnBlockIsWellFormed)
{
    Records r;
    r.computeEncoder(0);
    EXPECT_TRUE(r.record(Records::StartIf));
    EXPECT_TRUE(r.record(Records::StartElse));
    EXPECT_TRUE(r.record(Records::StartIf));
    EXPECT_TRUE(r.record(Records::EndIf));
    EXPECT_TRUE(r.record(Records::EndIf));
    EXPECT_FALSE(r.misused());
    EXPECT_FALSE(r.loops());
    EXPECT_TRUE(r.record(Records::StartWhile));
    EXPECT_FALSE(r.record(Records::EndIf));
    EXPECT_TRUE(r.misused());
    EXPECT_TRUE(r.loops());
}

TEST(GM20BPredicateRecords, ASecondElseOrAnElseOnALoopIsMisuse)
{
    Records a;
    a.computeEncoder(0);
    a.record(Records::StartIf);
    a.record(Records::StartElse);
    EXPECT_FALSE(a.record(Records::StartElse));
    EXPECT_TRUE(a.misused());
    Records b;
    b.computeEncoder(0);
    b.record(Records::StartWhile);
    EXPECT_FALSE(b.record(Records::StartElse));
    EXPECT_TRUE(b.misused());
}

TEST(GM20BPredicateRecords, AUnitEndsAtOtherWorkOrAnotherDispatchType)
{
    Records r;
    r.computeEncoder(0);
    r.record(Records::StartIf);
    r.computeEncoder(0);
    EXPECT_EQ(r.depth(), 1u);
    r.computeEncoder(1);
    EXPECT_EQ(r.depth(), 0u);
    r.record(Records::StartIf);
    r.otherWork();
    EXPECT_FALSE(r.record(Records::EndIf));
    EXPECT_TRUE(r.recorded());
}

TEST(GM20BPredicateRecords, ALoopRunsAcrossItsUnitsComputeEncodersButNotPastItsUnit)
{
    Records same;
    same.computeEncoder(0);
    same.record(Records::StartWhile);
    same.computeEncoder(0);
    EXPECT_TRUE(same.record(Records::EndWhile));
    same.end();
    EXPECT_FALSE(same.misused());
    for (bool otherWork : {false, true}) {
        Records ended;
        ended.computeEncoder(0);
        ended.record(Records::StartWhile);
        if (otherWork)
            ended.otherWork();
        else
            ended.computeEncoder(1);
        EXPECT_TRUE(ended.misused()) << otherWork;
    }
}

}  // namespace
