// SPDX-License-Identifier: MIT
// raw's predicated loops (raw_loop_copies.h) laid down as the compute encoder lays them and run GP entry by GP entry on
// predication_model.h, with launches that change the words the loops test. Every program runs twice: once with a
// fetcher that sees the heads the copies write, once with one that does not, and both must launch what Metal does.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/test/loop_program.h"

namespace {

using namespace loop_program;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// while (word 0 != 0) { launch 2: word 0 -= 1 } between launches 1 and 3
struct CountDown {
    LoopProgram prog;
    uint32_t body = 0;

    explicit CountDown(uint32_t count)
    {
        prog.gpu.put(word(0), count);
        prog.dispatch();
        EXPECT_EQ(prog.startWhile(0, PredicateCompare::NotEqual, 0), Answer::Done);
        body = prog.dispatch();
        EXPECT_EQ(prog.endWhile(), Answer::Done);
        prog.dispatch();
    }

    std::vector<uint32_t> run(bool seesHeads)
    {
        return prog.run(seesHeads, [this](uint32_t id, Gpu &gpu) {
            if (id == body)
                gpu.put(word(0), gpu.word(word(0)) - 1);
        });
    }
};

TEST(RawPredicateLoops, CopiesAreTheBoundForTheNestAndItsGpEntryBudget)
{
    EXPECT_EQ(raw::LoopCopies::copies(1, 1), 1024u);
    EXPECT_EQ(raw::LoopCopies::copies(1, 5), 819u);
    EXPECT_EQ(raw::LoopCopies::copies(2, 1), 128u);
    EXPECT_EQ(raw::LoopCopies::copies(2, 200), 20u);
    EXPECT_EQ(raw::LoopCopies::copies(3, 1), 16u);
    EXPECT_EQ(raw::LoopCopies::copies(4, 1), 2u);
    EXPECT_EQ(raw::LoopCopies::copies(raw::LoopCopies::kMaxDepth, 1), 1u);
    EXPECT_EQ(raw::LoopCopies::copies(raw::LoopCopies::kMaxDepth + 1, 1), 0u);
    EXPECT_EQ(raw::LoopCopies::copies(1, 5000), 0u);
}

TEST(RawPredicateLoops, AWhileRunsItsBodyUntilItsWordFailsUpToItsCopies)
{
    const uint32_t copies = raw::LoopCopies::kOutermostCopies;
    for (uint32_t count : {0u, 1u, 2u, 9u, copies - 1, copies}) {
        for (bool sees : {true, false}) {
            CountDown loop(count);
            EXPECT_EQ(loop.run(sees), repeat({1}, 2, count, {3})) << "count " << count << " sees " << sees;
            EXPECT_FALSE(loop.prog.overflowed()) << "count " << count;
            EXPECT_TRUE(loop.prog.qmdsIntact);
            EXPECT_EQ(loop.prog.gpu.word(word(0)), 0u);
        }
    }
}

TEST(RawPredicateLoops, PastItsCopiesTheRecordNamesTheCommandBufferAndTheLoop)
{
    const uint32_t copies = raw::LoopCopies::kOutermostCopies;
    for (bool sees : {true, false}) {
        CountDown loop(copies + 3);
        EXPECT_EQ(loop.run(sees), repeat({1}, 2, copies, {3}));
        EXPECT_TRUE(loop.prog.overflowed());
        EXPECT_EQ(loop.prog.record().tag, 0u);
    }
}

// Past its copies an inner loop records itself and stops what follows it in its blocks: the rest of the outer loop's
// copy and its later copies, or the rest of the If and its Else; the submission lays them again (raw_loop_ranges.h)
TEST(RawPredicateLoops, ALoopInsideAnotherOrABlockPastItsCopiesStopsWhatFollowsInThem)
{
    for (bool inLoop : {true, false}) {
        for (bool sees : {true, false}) {
            LoopProgram prog;
            prog.gpu.put(word(0), 2);
            prog.gpu.put(word(2), 0);
            if (inLoop)
                ASSERT_EQ(prog.startWhile(0, PredicateCompare::NotEqual, 0), Answer::Done);
            else
                prog.startIf(2, PredicateCompare::Equal, 0);
            const uint32_t outer = prog.dispatch();
            ASSERT_EQ(prog.startWhile(1, PredicateCompare::NotEqual, 0), Answer::Done);
            const uint32_t inner = prog.dispatch();
            ASSERT_EQ(prog.endWhile(7), Answer::Done);
            const uint32_t after = prog.dispatch();
            if (inLoop) {
                ASSERT_EQ(prog.endWhile(9), Answer::Done);
            } else {
                prog.startElse();
                prog.dispatch();
                prog.endIf();
            }
            const auto launched = prog.run(sees, [&](uint32_t id, Gpu &gpu) {
                if (id == outer) {
                    gpu.put(word(0), gpu.word(word(0)) - 1);
                    gpu.put(word(1), 1000);
                }
            });
            const uint32_t copies = inLoop ? raw::LoopCopies::kNestedCopies : raw::LoopCopies::kOutermostCopies;
            EXPECT_EQ(launched, repeat({outer}, inner, copies, {}))
                << (inLoop ? "inside a loop" : "inside an If") << " sees " << sees;
            EXPECT_EQ(std::count(launched.begin(), launched.end(), after), 0);
            EXPECT_TRUE(prog.overflowed());
            EXPECT_EQ(prog.record().tag, 7u);
        }
    }
}

TEST(RawPredicateLoops, TheCopiesAfterTheEndAreOneFetchEachWhereTheFetcherSeesTheHeads)
{
    CountDown seen(3), unseen(3);
    seen.run(true);
    unseen.run(false);
    const uint64_t others = unseen.prog.gpu.decoded - raw::LoopCopies::kOutermostCopies;
    EXPECT_EQ(seen.prog.gpu.decoded, others + 4);  // the three that ran and the one whose test failed
}

TEST(RawPredicateLoops, CannysWhileAboveAReferenceExitsOnTheOrderedCompare)
{
    for (bool sees : {true, false}) {
        LoopProgram prog;
        prog.gpu.put(word(0), 9000);
        ASSERT_EQ(prog.startWhile(0, PredicateCompare::Greater, 4096), Answer::Done);
        const uint32_t setup = prog.dispatch(), pass = prog.dispatch();
        ASSERT_EQ(prog.endWhile(), Answer::Done);
        const auto launched = prog.run(sees, [&](uint32_t id, Gpu &gpu) {
            if (id == pass)
                gpu.put(word(0), gpu.word(word(0)) - 1000);
        });
        EXPECT_EQ(launched, (std::vector<uint32_t>{setup, pass, setup, pass, setup, pass, setup, pass, setup, pass}));
        EXPECT_EQ(prog.gpu.word(word(0)), 4000u);
        EXPECT_FALSE(prog.overflowed());
    }
}

TEST(RawPredicateLoops, AnIfInsideACopyTestsItsWordAgainInEachCopy)
{
    for (bool sees : {true, false}) {
        LoopProgram prog;
        prog.gpu.put(word(0), 5);
        prog.gpu.put(word(1), 0);
        ASSERT_EQ(prog.startWhile(0, PredicateCompare::NotEqual, 0), Answer::Done);
        const uint32_t step = prog.dispatch();
        prog.startIf(1, PredicateCompare::Equal, 1);
        const uint32_t odd = prog.dispatch();
        prog.startElse();
        const uint32_t even = prog.dispatch();
        prog.endIf();
        ASSERT_EQ(prog.endWhile(), Answer::Done);
        const auto launched = prog.run(sees, [&](uint32_t id, Gpu &gpu) {
            if (id == step) {
                gpu.put(word(0), gpu.word(word(0)) - 1);
                gpu.put(word(1), gpu.word(word(1)) ^ 1);
            }
        });
        EXPECT_EQ(launched, (std::vector<uint32_t>{step, odd, step, even, step, odd, step, even, step, odd}));
        EXPECT_TRUE(prog.qmdsIntact);
    }
}

TEST(RawPredicateLoops, ALoopInABlockNotTakenRunsNothing)
{
    for (bool sees : {true, false}) {
        LoopProgram prog;
        prog.gpu.put(word(0), 4);
        prog.gpu.put(word(1), 1);
        prog.startIf(1, PredicateCompare::Equal, 0);
        ASSERT_EQ(prog.startWhile(0, PredicateCompare::NotEqual, 0), Answer::Done);
        prog.dispatch();
        ASSERT_EQ(prog.endWhile(), Answer::Done);
        prog.endIf();
        const uint32_t after = prog.dispatch();
        EXPECT_EQ(prog.run(sees), (std::vector<uint32_t>{after}));
        EXPECT_FALSE(prog.overflowed());
    }
}

TEST(RawPredicateLoops, ALoopInsideALoopIsArmedAgainForEachOuterCopy)
{
    for (bool sees : {true, false}) {
        LoopProgram prog;
        prog.gpu.put(word(0), 3);
        ASSERT_EQ(prog.startWhile(0, PredicateCompare::NotEqual, 0), Answer::Done);
        const uint32_t outer = prog.dispatch();
        ASSERT_EQ(prog.startWhile(1, PredicateCompare::NotEqual, 0), Answer::Done);
        const uint32_t inner = prog.dispatch();
        ASSERT_EQ(prog.endWhile(), Answer::Done);
        const uint32_t tail = prog.dispatch();
        ASSERT_EQ(prog.endWhile(), Answer::Done);
        const uint32_t after = prog.dispatch();
        uint32_t round = 0;
        const auto launched = prog.run(sees, [&](uint32_t id, Gpu &gpu) {
            if (id == outer) {
                gpu.put(word(0), gpu.word(word(0)) - 1);
                gpu.put(word(1), ++round);
            } else if (id == inner) {
                gpu.put(word(1), gpu.word(word(1)) - 1);
            }
        });
        std::vector<uint32_t> expected;
        for (uint32_t r = 1; r <= 3; ++r) {
            expected.push_back(outer);
            expected.insert(expected.end(), r, inner);
            expected.push_back(tail);
        }
        expected.push_back(after);
        EXPECT_EQ(launched, expected);
        EXPECT_FALSE(prog.overflowed());
    }
}

TEST(RawPredicateLoops, ADoWhileRunsItsBodyOnceBeforeItsFirstTest)
{
    for (uint32_t from : {0u, 5u})
        for (bool sees : {true, false}) {
            LoopProgram prog;
            prog.gpu.put(word(0), from);
            ASSERT_EQ(prog.startDoWhile(), Answer::Done);
            const uint32_t body = prog.dispatch();
            ASSERT_EQ(prog.endDoWhile(0, PredicateCompare::Less, 3), Answer::Done);
            const auto launched = prog.run(sees, [&](uint32_t id, Gpu &gpu) {
                if (id == body)
                    gpu.put(word(0), gpu.word(word(0)) + 1);
            });
            EXPECT_EQ(launched, std::vector<uint32_t>(from < 3 ? 3 - from : 1, body)) << "from " << from;
            EXPECT_FALSE(prog.overflowed());
        }
}

TEST(RawPredicateLoops, ABodyOverSeveralSegmentsEndsInEachOfThem)
{
    for (bool sees : {true, false}) {
        LoopProgram prog;
        prog.gpu.put(word(0), 4);
        ASSERT_EQ(prog.startWhile(0, PredicateCompare::NotEqual, 0), Answer::Done);
        const uint32_t first = prog.dispatch();
        prog.nextChunk();
        const uint32_t second = prog.dispatch();
        ASSERT_EQ(prog.endWhile(), Answer::Done);
        const auto launched = prog.run(sees, [&](uint32_t id, Gpu &gpu) {
            if (id == second)
                gpu.put(word(0), gpu.word(word(0)) - 1);
        });
        EXPECT_EQ(launched, (std::vector<uint32_t>{first, second, first, second, first, second, first, second}));
    }
}

TEST(RawPredicateLoops, BatchesEndBetweenCopiesNeverInsideOne)
{
    LoopProgram prog;
    prog.gpu.put(word(0), 1);
    const size_t bodyAt = prog.entries.size();
    ASSERT_EQ(prog.startWhile(0, PredicateCompare::NotEqual, 0), Answer::Done);
    prog.dispatch(raw::BatchWork::kLimit / 2);
    prog.nextChunk();
    prog.dispatch(1);
    ASSERT_EQ(prog.endWhile(), Answer::Done);
    const size_t body = 2;
    size_t spans = 0;
    for (size_t at = bodyAt; at < bodyAt + body * raw::LoopCopies::kOutermostCopies;) {
        const size_t n = prog.entries.next(at, 1u << 20);
        ASSERT_GT(n, 0u);
        if (at + n < bodyAt + body * raw::LoopCopies::kOutermostCopies)
            ASSERT_EQ((at + n - bodyAt) % body, 0u) << "a batch ends inside a copy";
        at += n;
        ++spans;
    }
    EXPECT_GE(spans, size_t(raw::LoopCopies::kOutermostCopies / 2));
}

TEST(RawPredicateLoops, WhatTheLayoutCannotRunIsMisused)
{
    LoopProgram depth;
    for (size_t d = 0; d < raw::LoopCopies::kMaxDepth; ++d)
        ASSERT_EQ(depth.startWhile(uint32_t(d), PredicateCompare::Equal, 1), Answer::Done) << d;
    EXPECT_EQ(depth.startWhile(7, PredicateCompare::Equal, 1), Answer::Misused);

    LoopProgram foreign;
    ASSERT_EQ(foreign.startWhile(0, PredicateCompare::Equal, 1), Answer::Done);
    foreign.dispatch();
    foreign.entries.append({kBase + 0x80000, 4, 0}, false);
    EXPECT_EQ(foreign.endWhile(), Answer::Misused);

    LoopProgram segments;
    ASSERT_EQ(segments.startWhile(0, PredicateCompare::Equal, 1), Answer::Done);
    for (size_t i = 0; i < raw::LoopCopies::kMaxSegments; ++i)
        segments.nextChunk();
    EXPECT_EQ(segments.endWhile(), Answer::Misused);

    LoopProgram none;
    EXPECT_EQ(none.endWhile(), Answer::Misused);
}

// The emitters write into room their callers reserve (and the arm into words reserved before the copies were laid), so
// each worst case must fit: an ordered compare (two copies and a reduction) inside enclosing blocks, the most heads
TEST(RawPredicateLoops, EveryEmitterStaysInsideTheRoomItsCallerReserves)
{
    static_assert(raw::LoopCopies::kStartRoom >= raw::Predication::kStartWords + 1);
    static_assert(raw::LoopCopies::endRoom(2) >=
                  raw::Predication::kEndCopyWords(raw::LoopCopies::kMaxSegments) + raw::Predication::kOverflowWords(1));
    uint64_t heads[raw::LoopCopies::kMaxSegments];
    for (size_t i = 0; i < raw::LoopCopies::kMaxSegments; ++i)
        heads[i] = kPush + 0x100 * i;
    const raw::PredicateTest ordered = raw::predicateTest(PredicateCompare::Greater, 7);
    for (bool doWhile : {false, true}) {
        LoopProgram outermost;
        outermost.halting();
        uint32_t *at = outermost.p.at;
        ASSERT_EQ(outermost.predication.startWhile(outermost.p, outermost.slot(), word(0), ordered), Answer::Done);
        EXPECT_LE(uint32_t(outermost.p.at - at), raw::Predication::kStartWords) << "an outermost While, halt gated";
        LoopProgram prog;
        prog.halting();
        prog.startIf(5, PredicateCompare::Equal, 0);
        const raw::PredicateSlot loop = prog.slot();
        uint32_t *before = prog.p.at;
        ASSERT_EQ(doWhile ? prog.predication.startDoWhile(prog.p, loop)
                          : prog.predication.startWhile(prog.p, loop, word(0), ordered),
                  Answer::Done);
        EXPECT_LE(uint32_t(prog.p.at - before), raw::Predication::kStartWords);
        before = prog.p.at;
        prog.startIf(6, PredicateCompare::LessEqual, 3);
        EXPECT_LE(uint32_t(prog.p.at - before), raw::Predication::kStartWords)
            << "an If inside a copy, with its resets";
        before = prog.p.at;
        prog.startElse();
        EXPECT_LE(uint32_t(prog.p.at - before), raw::Predication::kStartWords);
        prog.endIf();
        before = prog.p.at;
        ASSERT_EQ(prog.predication.endCopy(prog.p, heads, raw::LoopCopies::kMaxSegments, word(0),
                                           doWhile ? &ordered : nullptr),
                  Answer::Done);
        EXPECT_LE(uint32_t(prog.p.at - before), raw::Predication::kEndCopyWords(raw::LoopCopies::kMaxSegments));
        before = prog.p.at;
        const size_t enclosing = prog.predication.depth() - 1;
        prog.predication.endLoop(prog.p, kRecord, kStamp, 3);
        EXPECT_LE(uint32_t(prog.p.at - before), raw::Predication::kOverflowWords(enclosing));
        before = prog.p.at;
        raw::Predication::armCopies(prog.p, heads, raw::LoopCopies::kMaxSegments, raw::Predication::goingOf(loop));
        EXPECT_LE(uint32_t(prog.p.at - before), raw::Predication::kArmWords(raw::LoopCopies::kMaxSegments));
    }
}

// A command buffer with loops is one driver's but not one range (a wait or an un-inlined signal is a range of its own),
// so the overflow word is read with the stamp of the range that laid the loops, carried on its retirement
TEST(RawPredicateLoops, EachRawRangeThatLaidLoopsCarriesItsOwnStampToTheCompletion)
{
    const std::string encode = SourceText("../raw/runtime/RawMetalEncode.mm");
    EXPECT_THAT(encode, HasSubstr("cb->_rangeLoopStamp = rc->loopsLaid ? rc->stamp : 0;"));
    const std::string finish = SourceText("../raw/runtime/RawMetalFinish.mm");
    EXPECT_THAT(finish, HasSubstr("std::exchange(cb->_rangeLoopStamp, 0u)"));
    EXPECT_THAT(finish, HasSubstr("r.raw->loopContinue(retired.loopStamp)"));
}

}  // namespace
