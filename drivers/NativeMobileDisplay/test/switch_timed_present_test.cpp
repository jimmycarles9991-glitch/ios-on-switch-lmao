// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchTimedPresent.h: a swap flips at the frame end before its requested presentation, never earlier, and a swap
// stamped for the next vblank (every Core Animation frame) or with no target flips exactly as before.
#include <gtest/gtest.h>

#include <string>

#include "../SwitchTimedPresent.h"
#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;
using switch_timed_present::Clock;
using switch_timed_present::decide;
using switch_timed_present::Decision;

namespace {
constexpr uint64_t kPeriod = 16667, kHorizon = 1000000;  // microsecond ticks keep the arithmetic readable
constexpr uint64_t kFrameEnd = 1000000;

Clock at(uint64_t now, uint64_t asleep = 0, uint64_t lastFrameEnd = kFrameEnd, uint64_t accepted = 0)
{
    return {now, now + asleep, lastFrameEnd, kPeriod, kHorizon, accepted};
}

TEST(TimedPresent, NoTargetOrAPastOneIsDue)
{
    EXPECT_EQ(decide(0, at(kFrameEnd + 3000)), Decision::Due);
    EXPECT_EQ(decide(kFrameEnd - 5000, at(kFrameEnd + 3000)), Decision::Due);
}

// Core Animation stamps a frame with the vsync it rendered for: the next frame end (minus the vsync's lead).
TEST(TimedPresent, TheNextVblanksFrameFlipsAsBefore)
{
    for (uint64_t into : {0ULL, 2000ULL, 9000ULL, 16000ULL}) {
        const uint64_t now = kFrameEnd + into;
        EXPECT_EQ(decide(kFrameEnd + kPeriod, at(now)), Decision::Due) << into;
        EXPECT_EQ(decide(kFrameEnd + kPeriod - 1500, at(now)), Decision::Due) << into;
    }
}

// The render server's own timer sits at any phase to DC A's frame end: its frame for its next tick goes at once even when
// that tick is most of a period after the frame end that will show it.
TEST(TimedPresent, AnOrdinaryFrameGoesAtAnyTimerPhase)
{
    const uint64_t tick = kFrameEnd + 10000, now = tick + 1500;
    EXPECT_EQ(decide(tick + kPeriod, at(now)), Decision::Early);
    EXPECT_TRUE(switch_timed_present::due(decide(tick + kPeriod, at(now, 0, kFrameEnd, now - 200))));
    EXPECT_EQ(decide(tick + 2 * kPeriod, at(now, 0, kFrameEnd, now - 200)), Decision::Early);
}

// The ordinary-frame clause latches a swap at a frame end more than half a period before its requested presentation:
// it says so (Ordinary), for the debug count, and stays due. A swap the next frame end suits, or one decided without a
// phase, is plainly Due.
TEST(TimedPresent, TheOrdinaryClauseIsNamedWhereItShowsAFrameEarly)
{
    const uint64_t tick = kFrameEnd + 10000, now = tick + 1500, accepted = now - 200;
    EXPECT_EQ(decide(tick + kPeriod, at(now, 0, kFrameEnd, accepted)), Decision::Ordinary);
    EXPECT_TRUE(switch_timed_present::due(Decision::Ordinary));
    EXPECT_FALSE(switch_timed_present::due(Decision::Early));
    EXPECT_FALSE(switch_timed_present::due(Decision::NeedsPhase));
    EXPECT_EQ(decide(kFrameEnd + kPeriod, at(now, 0, kFrameEnd, accepted)), Decision::Due);
    const uint64_t stale = kFrameEnd + 10 * kPeriod + 1500;
    EXPECT_EQ(decide(stale + kPeriod, at(stale, 0, kFrameEnd, stale - 200)), Decision::Due);
    EXPECT_EQ(decide(stale + kPeriod, at(stale)), Decision::NeedsPhase);
}

// The display treats the clause's swaps as due and counts each once (debug kernels), when the kick takes it: a kick
// that defers it for another reason asks the gate again, which must not count it twice.
TEST(TimedPresentSource, TheDisplayCountsAnOrdinarySwapOnceAtItsTake)
{
    const std::string source = SourceText("../SwitchTimedPresentDisplay.h");
    const CodeText gate = Block(source, "bool presentNotDue(");
    EXPECT_TRUE(gate.before("Decision::Ordinary ? request : nullptr", "if (switch_timed_present::due(decision))"));
    EXPECT_FALSE(gate.has("timedOrdinary"));
    EXPECT_TRUE(Block(source, "void noteOrdinaryTaken(").has("++frameInfo->pending.timedOrdinary;"));
    EXPECT_TRUE(Block(SourceText("../NativeLegacyDisplay.cpp"), "void noteFrameInfoTaken(")
                    .before("noteOrdinaryTaken(request);", "frameInfo->pending.take("));
}

TEST(TimedPresent, ATargetTwoFramesOutWaitsForTheFrameEndBeforeIt)
{
    const uint64_t target = kFrameEnd + 2 * kPeriod;
    EXPECT_EQ(decide(target, at(kFrameEnd + 4000, 0, kFrameEnd, kFrameEnd + 3000)), Decision::Early);
    // at the next frame end the one after is the target's
    EXPECT_EQ(decide(target, at(kFrameEnd + kPeriod + 200, 0, kFrameEnd + kPeriod)), Decision::Due);
}

TEST(TimedPresent, HalfAPeriodDecidesWhichFrameEnd)
{
    const uint64_t now = kFrameEnd + 1000;
    EXPECT_EQ(decide(kFrameEnd + kPeriod + kPeriod / 2, at(now)), Decision::Due);
    EXPECT_EQ(decide(kFrameEnd + kPeriod + kPeriod / 2 + 1, at(now)), Decision::Early);
}

// Frame ends masked while idle leave the last stamp stale: a future target waits for a fresh one rather than guess the
// phase, and a target within half a period of now needs no phase at all.
TEST(TimedPresent, AStaleFrameEndAsksForTheNextOne)
{
    const uint64_t now = kFrameEnd + 10 * kPeriod + 3000;
    EXPECT_EQ(decide(now + 3 * kPeriod, at(now)), Decision::NeedsPhase);
    EXPECT_EQ(decide(now + kPeriod / 2, at(now)), Decision::Due);
    EXPECT_EQ(decide(now + 3 * kPeriod, at(now, 0, 0)), Decision::NeedsPhase);
}

// The target is continuous time: after a system sleep it is ahead of absolute time by the time asleep.
TEST(TimedPresent, TheTargetIsInContinuousTime)
{
    const uint64_t asleep = 5000000, now = kFrameEnd + 2000;
    EXPECT_EQ(decide(kFrameEnd + kPeriod + asleep, at(now, asleep)), Decision::Due);
    EXPECT_EQ(decide(kFrameEnd + 3 * kPeriod + asleep, at(now, asleep)), Decision::Early);
    EXPECT_EQ(decide(asleep / 2, at(now, asleep)), Decision::Due);
}

TEST(TimedPresent, ATargetPastTheHorizonOrNoPeriodIsDue)
{
    const uint64_t now = kFrameEnd + 2000;
    EXPECT_EQ(decide(now + kHorizon + 1, at(now)), Decision::Due);
    Clock noPeriod = at(now);
    noPeriod.period = 0;
    EXPECT_EQ(decide(now + 5 * kPeriod, noPeriod), Decision::Due);
}

// The kick asks before it takes anything; a held swap sets kickDeferred, which keeps the frame-end interrupt routed and
// kicks it again at the next frame end (kickAtIdleFrameEnd).
TEST(TimedPresentSource, TheKickAsksFirstAndAFrameEndKicksAgain)
{
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    const CodeText kick = Block(display, "void kickOnce()");
    EXPECT_TRUE(kick.before("if (presentNotDue(request))", "planeDecide(request, pd);"));
    EXPECT_TRUE(kick.before("if (!request || !isValidated(request))", "if (presentNotDue(request))"));
    EXPECT_TRUE(CodeText(SourceText("../SwitchTimedPresentDisplay.h")).has("kickDeferred = true;"));
    EXPECT_TRUE(Block(display, "bool frameEndNeeded()").has("kickDeferred"));
    EXPECT_TRUE(CodeText(SourceText("../SwitchRetireDisplay.h")).has("kickDeferred = false; kickSwapQueue();"));
}
}  // namespace
