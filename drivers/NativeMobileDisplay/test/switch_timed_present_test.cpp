// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchTimedPresent.h: a swap flips at the frame end before its requested presentation, never earlier, and a swap
// stamped for the next vblank or with no target flips at once.
#include <gtest/gtest.h>

#include <string>
#include <vector>

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

Clock at(uint64_t now, uint64_t asleep = 0, uint64_t lastFrameEnd = kFrameEnd, uint64_t accepted = 0,
         bool phasedOrdinary = false)
{
    return {now, now + asleep, lastFrameEnd, kPeriod, kHorizon, accepted, phasedOrdinary};
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

// With the phase known the stamp alone decides, whatever the swap's acceptance time: a frame stamped for the second
// frame end ahead waits for the frame end before it, accepted early or late in the period.
TEST(TimedPresent, AcceptanceTimeDoesNotMoveAPhasedSwap)
{
    const uint64_t target = kFrameEnd + 2 * kPeriod;
    for (uint64_t into : {2000ULL, 9000ULL, 12000ULL, 16000ULL}) {
        const uint64_t now = kFrameEnd + into;
        EXPECT_EQ(decide(target, at(now, 0, kFrameEnd, now - 200)), Decision::Early) << into;
        EXPECT_EQ(decide(target, at(now)), Decision::Early) << into;
    }
}

// Decided without a phase (the frame ends were masked), a swap stamped for the next vsync (within a period and the wake
// jitter of its acceptance) is the render server's ordinary frame and goes at once (OrdinaryUnphased); without an
// acceptance time it waits for a frame end to learn the phase from.
TEST(TimedPresent, AnUnphasedOrdinaryFrameGoesAtOnce)
{
    EXPECT_FALSE(switch_timed_present::due(Decision::Early));
    EXPECT_FALSE(switch_timed_present::due(Decision::NeedsPhase));
    const uint64_t stale = kFrameEnd + 10 * kPeriod + 1500;
    EXPECT_EQ(decide(stale + kPeriod, at(stale, 0, kFrameEnd, stale - 200)), Decision::OrdinaryUnphased);
    EXPECT_TRUE(switch_timed_present::due(Decision::OrdinaryUnphased));
    EXPECT_EQ(decide(stale + kPeriod, at(stale)), Decision::NeedsPhase);
    EXPECT_EQ(decide(stale + 3 * kPeriod, at(stale, 0, kFrameEnd, stale - 200)), Decision::NeedsPhase);
}

// The first swap after an idle stretch: stamped two frame ends ahead and accepted 12 ms into the period, it waits a
// frame end for the phase instead of going a period early (the start-of-burst skip, review-display item 2).
TEST(TimedPresent, AnIdleLeadTwoSwapWaitsForThePhase)
{
    const uint64_t tick = kFrameEnd + 10 * kPeriod, accepted = tick + 12000;
    EXPECT_EQ(decide(tick + 2 * kPeriod, at(accepted + 100, 0, kFrameEnd, accepted)), Decision::NeedsPhase);
    // A stamp for the next vsync goes at once: under half a period away, or the render server's ordinary frame.
    EXPECT_EQ(decide(tick + kPeriod, at(accepted + 100, 0, kFrameEnd, accepted)), Decision::Due);
    const uint64_t early = tick + 3000;
    EXPECT_EQ(decide(tick + kPeriod, at(early + 100, 0, kFrameEnd, early)), Decision::OrdinaryUnphased);
}

// A frame end the filter stamped after the clock was read is taken as now, not as a lost phase.
TEST(TimedPresent, AFrameEndNewerThanNowIsNow)
{
    const uint64_t now = kFrameEnd + 3 * kPeriod + 12000;
    EXPECT_EQ(decide(now + 2 * kPeriod, at(now, 0, now + 40, now - 200)), Decision::Early);
}

// The phase holds for two periods of frame ends and is lost at the second; the next frame end regains it.
TEST(TimedPresent, ThePhaseIsLostAtTwoPeriodsAndRegainedAtTheNextFrameEnd)
{
    const uint64_t target = kFrameEnd + 4 * kPeriod;
    const uint64_t before = kFrameEnd + 2 * kPeriod - 1, at2 = kFrameEnd + 2 * kPeriod;
    EXPECT_EQ(decide(target, at(before)), Decision::Early);
    EXPECT_EQ(decide(target, at(at2)), Decision::NeedsPhase);
    EXPECT_EQ(decide(target, at(at2 + kPeriod + 50, 0, at2 + kPeriod)), Decision::Due);
}

// 21H461 keeps the ordinary-frame clause (kPhasedOrdinary): a phased swap within 1.5 periods of its acceptance goes at
// once, and its unphased span stays 1.5 periods. 24B119's stamp alone decides.
TEST(TimedPresent, The21H461ClauseIsKeptWhereItIsAsked)
{
    const uint64_t tick = kFrameEnd + 10000, now = tick + 1500, accepted = now - 200;
    EXPECT_EQ(decide(tick + kPeriod, at(now, 0, kFrameEnd, accepted, true)), Decision::Ordinary);
    EXPECT_TRUE(switch_timed_present::due(Decision::Ordinary));
    EXPECT_EQ(decide(tick + kPeriod, at(now, 0, kFrameEnd, accepted, false)), Decision::Early);
    const uint64_t idle = kFrameEnd + 10 * kPeriod, late = idle + 12000;
    EXPECT_EQ(decide(idle + 2 * kPeriod, at(late + 100, 0, kFrameEnd, late, true)), Decision::OrdinaryUnphased);
}

// One swap per period from a render thread woken at each frame end, stamped `lead` frame ends ahead and accepted at a
// phase that varies frame to frame (the 25 Nov captures: 2.5-15 ms after the frame end). The display asks the gate at
// acceptance and again at every frame end while it holds the swap (kickAtIdleFrameEnd, ~80 us after it); a flip armed
// at t latches at the first frame end after t. Every frame must latch one period after the one before.
std::vector<uint64_t> replay(unsigned lead)
{
    const uint64_t accepts[] = {2500, 9500, 3000, 12000, 4000, 15000, 8500, 8000, 2600, 11000, 5000, 14500};
    std::vector<uint64_t> latched;
    for (unsigned k = 0; k < sizeof(accepts) / sizeof(accepts[0]); ++k) {
        const uint64_t frameEnd = kFrameEnd + k * kPeriod;
        const uint64_t accepted = frameEnd + accepts[k];
        const uint64_t requested = frameEnd + lead * kPeriod;
        uint64_t asked = accepted, lastFrameEnd = frameEnd;
        while (!switch_timed_present::due(decide(requested, at(asked, 0, lastFrameEnd, accepted)))) {
            lastFrameEnd += kPeriod;
            asked = lastFrameEnd + 80;
        }
        latched.push_back(kFrameEnd + ((asked - kFrameEnd) / kPeriod + 1) * kPeriod);
    }
    return latched;
}

TEST(TimedPresent, ConsecutiveFramesLatchOnePeriodApartAtEitherLead)
{
    for (unsigned lead : {1U, 2U}) {
        const std::vector<uint64_t> latched = replay(lead);
        for (size_t i = 0; i < latched.size(); ++i)
            EXPECT_EQ(latched[i], kFrameEnd + (i + lead) * kPeriod) << "lead " << lead << " frame " << i;
    }
}

// The display counts a swap the unphased clause let go once (debug kernels), when the kick takes it: a kick that
// defers it for another reason asks the gate again, which must not count it twice.
// The gate's answer is what kickAtIdleFrameEnd reads (headHeldForTime), set from the decision before the due return,
// per kind as built, and cleared when the kick takes the head.
TEST(TimedPresentSource, TheGateRecordsItsAnswerForTheFrameEndKick)
{
    const std::string source = SourceText("../SwitchTimedPresentDisplay.h");
    const CodeText gate = Block(source, "bool presentNotDue(");
    EXPECT_TRUE(gate.before("headHeldForTime = !switch_timed_present::due(decision);",
                            "if (switch_timed_present::due(decision)) return false;"));
    EXPECT_TRUE(gate.before("const uint64_t lastFrameEnd = __atomic_load_n(&feStamp", "mach_absolute_time()"));
    EXPECT_TRUE(CodeText(source).has("static constexpr bool kPhasedOrdinary = false;"));
    EXPECT_TRUE(
        Block(SourceText("../NativeLegacyDisplay.cpp"), "void noteFrameInfoTaken(").has("headHeldForTime = false;"));
}

TEST(TimedPresentSource, TheDisplayCountsAnUnphasedSwapOnceAtItsTake)
{
    const std::string source = SourceText("../SwitchTimedPresentDisplay.h");
    const CodeText gate = Block(source, "bool presentNotDue(");
    EXPECT_TRUE(
        gate.before("Decision::OrdinaryUnphased ? request : nullptr", "if (switch_timed_present::due(decision))"));
    EXPECT_FALSE(gate.has("timedUnphased"));
    EXPECT_TRUE(Block(source, "void noteUnphasedTaken(").has("++frameInfo->pending.timedUnphased;"));
    EXPECT_TRUE(Block(SourceText("../NativeLegacyDisplay.cpp"), "void noteFrameInfoTaken(")
                    .before("noteUnphasedTaken(request);", "frameInfo->pending.take("));
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
    EXPECT_TRUE(Block(display, "switch_frame_end::Holder frameEndHolder()").has("kickDeferred"));
    EXPECT_TRUE(CodeText(SourceText("../SwitchRetireDisplay.h")).has("kickDeferred = false; kickSwapQueue();"));
}
}  // namespace
