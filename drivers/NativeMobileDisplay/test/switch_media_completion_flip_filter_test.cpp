// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchMediaCompletionFlipDisplay.h's filter writes: host1x's syncpoint filter writes windows B and C's armed set once
// every producer's job is certainly done; the completion then writes nothing, a frame end or gate writer between
// adopts the set into the latch, and the judging and turn-off cover it (switch_media_completion_flip_harness.h).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "switch_media_completion_flip_harness.h"

namespace media_flip_test {
namespace {

constexpr uint32_t kStartB = Display::kWinbufBBase + native_planes::kStartAddr;
constexpr uint32_t kStartUB = Display::kWinbufBBase + native_planes::kStartAddrU;

class MediaFlipFilter : public testing::Test {
protected:
    Display d;
    uint64_t seq = 0;

    void SetUp() override
    {
        gLog.clear();
        gFenceResult = gWaitResult = 0;
        gProducerCertain = 1;
        gWaiters.clear();
        gUncertain.clear();
        gDoneAtArm.clear();
        shownByGate(video(1));
    }

    void frame()
    {
        d.model.boundary();
        d.frameEnd();
    }

    uint32_t address(int slot, unsigned plane = 0) const
    {
        const Display::PlaneSet p = video(slot);
        return d.windowAddress(p.window[0], plane);
    }

    void shownByGate(const Display::PlaneSet &p)
    {
        ASSERT_TRUE(d.offer(p, ++seq));
        d.complete(0);
        d.donePass();
        frame();
        ASSERT_EQ(d.shown.back(), seq);
    }

    bool armed(int slot)
    {
        return d.offer(video(slot), ++seq) && d.mediaFlip.state == SwitchCompletionFlip::kArmed;
    }
};

TEST_F(MediaFlipFilter, TheFilterWritesTheSetAndItsCompletionWritesNothing)
{
    EXPECT_EQ(gPreemptionOff, 0);
    ASSERT_TRUE(armed(2));
    ASSERT_EQ(gWaiters.size(), 1u) << "a waiter on the set's producer";
    d.model.written.clear();
    d.filter();
    const std::vector<uint32_t> want = {kStartB, kStartUB, Display::kDcStateControl, Display::kDcStateControl};
    EXPECT_EQ(d.model.written, want);
    EXPECT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kEarly);
    d.model.written.clear();
    d.filter();
    d.complete(0);
    EXPECT_TRUE(d.model.written.empty());
    d.donePass();
    EXPECT_TRUE(d.planeLatch.armedRequested);
    EXPECT_TRUE(d.mediaFlipIdle());
    frame();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_EQ(d.completions(seq), 1u) << "latched after its completion's pass: retired at the latch";
    EXPECT_TRUE(gLog.empty());
}

// The boundary takes the filter's set and the frame end runs before host1x's work loop delivers its completion: the
// latch has it at that frame end, and the completion's pass enters nothing and retires the swap the latch held back.
TEST_F(MediaFlipFilter, AFrameEndBeforeTheCompletionAdoptsTheSet)
{
    ASSERT_TRUE(armed(2));
    d.filter();
    frame();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kAdopted);
    d.model.written.clear();
    d.complete(0);
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_TRUE(d.mediaFlipIdle());
    EXPECT_EQ(d.completions(seq), 1u) << "the latch's retirement, held while the fence was out, at the pass";
    EXPECT_EQ(d.framesShown.back(), seq);
    frame();
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_EQ(d.completions(seq), 1u);
}

TEST_F(MediaFlipFilter, AGateWriterBeforeTheCompletionAdoptsTheSet)
{
    ASSERT_TRUE(armed(2));
    d.filter();
    d.mediaFlipFromGate();
    EXPECT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kAdopted);
    EXPECT_TRUE(d.planeLatch.armedRequested);
    d.complete(0);
    d.donePass();
    EXPECT_TRUE(d.mediaFlipIdle());
    frame();
    EXPECT_EQ(d.shown.back(), seq);
}

// Two producers, a waiter on each: the first threshold's filter finds the other job still running and writes nothing;
// the second's writes the set.
TEST_F(MediaFlipFilter, ATwoProducerSetWaitsForBothProducers)
{
    ASSERT_TRUE(d.offer(video(2), ++seq, 2));
    ASSERT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kArmed);
    ASSERT_EQ(gWaiters.size(), 2u);
    gUncertain = {seq + Display::kSecond};
    d.model.written.clear();
    d.filter(seq);
    EXPECT_TRUE(d.model.written.empty()) << "the second producer is still writing";
    gUncertain.clear();
    d.filter(seq + Display::kSecond);
    EXPECT_FALSE(d.model.written.empty());
    EXPECT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kEarly);
}

// A producer already done at the arm gets no waiter, and its record (perhaps gone) is not read in the filter.
TEST_F(MediaFlipFilter, AProducerDoneAtTheArmIsNotWaitedFor)
{
    gDoneAtArm = {seq + 1};
    gUncertain = {seq + 1};
    ASSERT_TRUE(d.offer(video(2), ++seq, 2));
    ASSERT_EQ(gWaiters.size(), 1u);
    EXPECT_EQ(gWaiters[0].threshold, static_cast<uint32_t>(seq + Display::kSecond));
    d.model.written.clear();
    d.filter();
    EXPECT_FALSE(d.model.written.empty());
}

TEST_F(MediaFlipFilter, AProducerNotCertainlyDoneIsLeftToTheCompletion)
{
    ASSERT_TRUE(armed(2));
    gProducerCertain = 0;
    d.model.written.clear();
    d.filter();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kArmed);
    d.complete(0);
    EXPECT_FALSE(d.model.written.empty());
}

// A waiter left from an earlier offer (a pass that beat its filter) claims nothing of the next offer's set.
TEST_F(MediaFlipFilter, AStaleWaiterClaimsNothingOfTheNextOffer)
{
    ASSERT_TRUE(armed(2));
    const auto stale = gWaiters;
    d.complete(0);
    d.donePass();
    frame();
    ASSERT_TRUE(armed(3));
    d.model.written.clear();
    for (const auto &waiter : stale)
        waiter.fn(waiter.ctx);
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kArmed);
}

// The producers' contract broken: a set the filter wrote whose completion then failed is shown as done, logged once,
// and the filter writes no set after it.
TEST_F(MediaFlipFilter, AFilterSetWhoseCompletionFailsIsShownAndTurnsTheFilterOff)
{
    ASSERT_TRUE(armed(2));
    d.filter();
    d.complete(-5);
    EXPECT_EQ(d.gpuStatus, 0);
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_NE(gLog[0].find("whose completion then failed (-5)"), std::string::npos) << gLog[0];
    d.donePass();
    frame();
    EXPECT_EQ(d.shown.back(), seq);
    ASSERT_TRUE(armed(3));
    d.model.written.clear();
    d.filter();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kArmed);
}

// A filter's stores that never reach the assembly copy are judged as the completion's: one line, these flips and the
// filter's writes off, the set from the gate.
TEST_F(MediaFlipFilter, AFilterSetThatDoesNotLatchIsWrittenFromTheGateAndTurnsTheFilterOff)
{
    ASSERT_TRUE(armed(2));
    d.model.completionDropsAddresses = true;
    d.filter();
    d.complete(0);
    d.donePass();
    ASSERT_NO_THROW(frame());
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_NE(gLog[0].find("windows B and C's completion flip did not latch"), std::string::npos);
    EXPECT_TRUE(d.mediaFlipsRefused);
    EXPECT_TRUE(d.mediaFilterOff);
    ASSERT_NO_THROW(frame());
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_FALSE(armed(3));
}

TEST_F(MediaFlipFilter, SuspendAdoptsAnEarlySet)
{
    ASSERT_TRUE(armed(2));
    d.filter();
    d.suspend();
    EXPECT_EQ(d.mediaFlip.state, SwitchCompletionFlip::kAdopted);
    d.complete(0);
    d.donePass();
    d.resume();
    EXPECT_TRUE(d.mediaFlipIdle());
    ASSERT_NO_THROW(frame());
    ASSERT_NO_THROW(frame());
    EXPECT_EQ(d.model.active[kStartB], address(2));
}

// The client's swap completes exactly once, whichever of the filter, the completion, a frame end and SC7 comes first,
// with nothing presented after a resume (only the display's own frame ends), and never before its frame is seen shown.
// A present after the resume completes nothing of it again.
TEST_F(MediaFlipFilter, TheSwapCompletesExactlyOnceWhateverOrderTheyRunIn)
{
    const char *const orders[] = {
        "filter completion pass frame",   "filter frame completion pass",          "completion pass frame",
        "completion frame pass",          "filter suspend completion pass resume", "filter gate completion pass",
        "completion suspend pass resume", "filter completion suspend pass resume", "completion pass suspend resume",
        "suspend completion pass resume",
    };
    for (const char *order : orders) {
        const std::string steps = order;
        ASSERT_TRUE(armed(2 + static_cast<int>(seq % 3))) << steps;
        const uint64_t mine = seq;
        for (size_t at = 0, next = 0; at < steps.size(); at = next + 1) {
            next = steps.find(' ', at);
            if (next == std::string::npos)
                next = steps.size();
            const std::string step = steps.substr(at, next - at);
            if (step == "filter")
                d.filter();
            else if (step == "completion")
                d.complete(0);
            else if (step == "pass")
                d.donePass();
            else if (step == "frame")
                frame();
            else if (step == "suspend")
                d.suspend();
            else if (step == "resume")
                d.resume();
            else if (step == "gate")
                d.mediaFlipFromGate();
            EXPECT_LE(d.completions(mine), 1u) << steps << ", after " << step;
        }
        frame();
        frame();
        EXPECT_TRUE(d.mediaFlipIdle()) << steps;
        EXPECT_EQ(d.completions(mine), 1u) << steps << ": with nothing presented after it";
        EXPECT_TRUE(std::count(d.framesShown.begin(), d.framesShown.end(), mine)) << steps << ": not seen shown";
        shownByGate(video(1 + static_cast<int>(seq % 2) * 4));
        EXPECT_EQ(d.completions(mine), 1u) << steps << ": with a present after it";
    }
    EXPECT_TRUE(gLog.empty());
}

// SC7 takes the requested set as shown: its swap completes at the settle, before any frame end after the resume.
TEST_F(MediaFlipFilter, ASetSettledAtSC7CompletesThereWithNothingPresentedAfter)
{
    ASSERT_TRUE(armed(2));
    d.complete(0);
    d.donePass();
    ASSERT_TRUE(d.planeLatch.requested());
    d.suspend();
    EXPECT_EQ(d.completions(seq), 1u);
    EXPECT_EQ(d.framesShown.back(), seq);
    d.resume();
    frame();
    EXPECT_EQ(d.completions(seq), 1u);
}

// The arm stages the set and its address stores under completionFlipLock, and host1x's filter writes it with preemption
// off: neither logs or blocks (IOLog throws with preemption off), and both leave preemption as they found it.
TEST_F(MediaFlipFilter, TheArmAndTheFilterRunNothingThatBlocks)
{
    bool ok = false;
    ASSERT_NO_THROW(ok = armed(2));
    ASSERT_TRUE(ok);
    EXPECT_EQ(gPreemptionOff, 0);
    d.model.written.clear();
    ASSERT_NO_THROW(d.filter());
    EXPECT_FALSE(d.model.written.empty());
    EXPECT_EQ(gPreemptionOff, 0);
    ++gPreemptionOff;
    EXPECT_THROW(IOLog("SwitchDisplay: a line"), std::runtime_error);
    --gPreemptionOff;
    d.complete(0);
    d.donePass();
    frame();
    EXPECT_EQ(d.shown.back(), seq);
}

}  // namespace
}  // namespace media_flip_test
