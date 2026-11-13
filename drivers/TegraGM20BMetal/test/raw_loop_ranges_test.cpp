// SPDX-License-Identifier: MIT
#include "drivers/TegraGM20BMetal/raw/runtime/raw_loop_ranges.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using Records = raw::PredicateRecords;
using Ranges = std::vector<raw::TapeRange>;
using Splits = std::vector<uint32_t>;

// A tape as the facade records it: 'C' a compute encoder (serial), 'P' a compute encoder (concurrent), 'R' other work,
// and the records: i (StartIf), e (StartElse), f (EndIf), w/W (StartWhile/EndWhile), d/D (StartDoWhile/EndDoWhile),
// '.' a dispatch
struct Tape {
    std::string text;

    raw::LoopTapeEntry operator()(uint32_t at) const
    {
        const char c = at < text.size() ? text[at] : '.';
        const Records::Kind kinds[] = {Records::StartIf,    Records::StartElse, Records::EndIf,
                                       Records::StartWhile, Records::EndWhile,  Records::StartDoWhile,
                                       Records::EndDoWhile};
        const std::string letters = "iefwWdD";
        if (c == 'C' || c == 'P')
            return {raw::LoopTapeEntry::Compute, c == 'C' ? 0u : 1u};
        if (c == 'R')
            return {raw::LoopTapeEntry::Other, 0};
        if (const size_t k = letters.find(c); k != std::string::npos)
            return {raw::LoopTapeEntry::Predicate, uint32_t(kinds[k])};
        return {};
    }

    uint32_t size() const
    {
        return uint32_t(text.size());
    }
};

Splits splits(const std::string &text)
{
    const Tape t{text};
    return raw::loopSplits(t, 0, t.size());
}

Ranges continuation(const std::string &text, uint32_t endAt, uint32_t from = 0)
{
    const Tape t{text};
    return raw::continuation(t, from, t.size(), endAt);
}

TEST(RawLoopRanges, EachBlockThatHoldsALoopAndNoBlockHoldsEndsARange)
{
    EXPECT_EQ(splits("C.w.W.d.D."), (Splits{5, 9}));
    EXPECT_EQ(splits("Cw.d.D.W."), (Splits{8}));
    EXPECT_EQ(splits("Ci.w.W.f."), (Splits{8}));
    EXPECT_EQ(splits("Ci.e.d.D.f."), (Splits{10}));
}

TEST(RawLoopRanges, BlocksWithoutLoopsSplitNothing)
{
    EXPECT_EQ(splits("C.i.e.f."), Splits{});
    EXPECT_EQ(splits("C.i.i.f.f."), Splits{});
}

// A block stays open across compute encoders of one dispatch type, and closes where another encoder or type starts
TEST(RawLoopRanges, BlocksCloseAsTheRecordsUnitsCloseThem)
{
    EXPECT_EQ(splits("Ci.Cw.W.f."), (Splits{9}));
    EXPECT_EQ(splits("Ci.w.W.R."), (Splits{7}));
    EXPECT_EQ(splits("Ci.w.W.P."), (Splits{7}));
}

TEST(RawLoopRanges, OnlyRangesWithALoopStartLayLoops)
{
    const Tape t{"C.i.f.w.W."};
    EXPECT_FALSE(raw::laysLoops(t, 0, 6));
    EXPECT_TRUE(raw::laysLoops(t, 0, 10));
}

TEST(RawLoopRanges, ALoopNoBlockHoldsContinuesAlone)
{
    // C . w . W .
    // 0 1 2 3 4 5
    EXPECT_EQ(continuation("C.w.W.", 4), (Ranges{{2, 5}}));
}

// The loop, then the rest of its If up to the Else (the If ran, so the Else does not); the If itself is not laid again
TEST(RawLoopRanges, ALoopInAnIfsThenPartContinuesWithTheRestOfThatPart)
{
    // C i . w . W . e . f .
    // 0 1 2 3 4 5 6 7 8 9 10
    EXPECT_EQ(continuation("Ci.w.W.e.f.", 5), (Ranges{{3, 6}, {6, 7}}));
}

TEST(RawLoopRanges, ALoopInAnElseContinuesWithTheRestOfTheElse)
{
    // C i . e w . W . f
    // 0 1 2 3 4 5 6 7 8
    EXPECT_EQ(continuation("Ci.ew.W.f", 6), (Ranges{{4, 7}, {7, 8}}));
}

// The inner loop, the rest of the outer one's body, then the outer loop again from its start (it tests its word)
TEST(RawLoopRanges, AnInnerLoopContinuesWithTheRestOfTheOuterOneAndTheOuterOneAgain)
{
    // C w . w . W . W .
    // 0 1 2 3 4 5 6 7 8
    EXPECT_EQ(continuation("Cw.w.W.W.", 5), (Ranges{{3, 6}, {6, 7}, {1, 8}}));
}

TEST(RawLoopRanges, ALoopThreeBlocksDeepUnwindsEachOfThem)
{
    // C i w d . D . W . e .  f
    // 0 1 2 3 4 5 6 7 8 9 10 11
    EXPECT_EQ(continuation("Ciwd.D.W.e.f", 5), (Ranges{{3, 6}, {6, 7}, {2, 8}, {8, 9}}));
}

// A block closed by its unit's end ends at the record that closed it
TEST(RawLoopRanges, ABlockItsUnitClosesEndsWhereTheUnitDoes)
{
    // C i . w . W . R .
    // 0 1 2 3 4 5 6 7 8
    EXPECT_EQ(continuation("Ci.w.W.R.", 5), (Ranges{{3, 6}, {6, 7}}));
}

TEST(RawLoopRanges, ARecordOfNoLoopContinuesNothing)
{
    EXPECT_EQ(continuation("C.w.W.", 3), Ranges{});
    EXPECT_EQ(continuation("C.i.f.", 4), Ranges{});
}

// A range laid as a continuation piece starts at no block: a loop in it continues within it
TEST(RawLoopRanges, ALoopOfAPieceContinuesWithinThePiece)
{
    // C w . w . W . i w . W  f  W
    // 0 1 2 3 4 5 6 7 8 9 10 11 12
    EXPECT_EQ(continuation("Cw.w.W.iw.WfW", 10, 6), (Ranges{{8, 11}}));
}

TEST(RawLoopRanges, ARecordNamesTheCommandBufferWhoseStampItHolds)
{
    EXPECT_TRUE(raw::loopContinue(5, 5, 9).continues);
    EXPECT_EQ(raw::loopContinue(5, 5, 9).tag, 9u);
    EXPECT_FALSE(raw::loopContinue(5, 4, 9).continues);
    EXPECT_FALSE(raw::loopContinue(0, 0, 9).continues);
}

}  // namespace
