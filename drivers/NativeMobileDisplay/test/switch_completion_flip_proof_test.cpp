// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchCompletionFlipDisplay.h on a unit whose window A WINBUF aperture refuses the completion's stores (lost, or
// kept where WIN_A_UPDATE does not take them): a frame end that shows such a flip's request taken with window A
// elsewhere logs one line and flips that frame from the gate, and the next frame is armed as usual; the ring never reads
// a latch it cannot explain. No such unit exists (TRM 24.10-24.11; display.completion_flip winbuf_alias): the model keeps
// the latch check honest.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "switch_completion_flip_harness.h"
#include "switch_completion_flip_walk.h"

namespace completion_flip_test {
namespace {

constexpr int kSlot = Display::kSlot0;
constexpr uint32_t kApertureFirst = 0xb80, kApertureLast = 0xbf9;

std::string refusal(uint32_t wrote, uint32_t active)
{
    char line[256];
    snprintf(line, sizeof(line),
             "SwitchDisplay: window A's completion flip did not latch (wrote START_ADDR %#x, active %#x, STATE_CONTROL "
             "%#x): this frame flips from the display's work loop\n",
             wrote, active, 0u);
    return line;
}

class Refusing : public testing::TestWithParam<DcModel::Aperture> {
protected:
    Display d;

    void SetUp() override
    {
        gOffer = nullptr;
        gLog.clear();
        d.model.winbuf = GetParam();
    }

    void frame()
    {
        d.boundary();
        d.tick();
    }

    // A GPU or VIC copy into the next scan buffer, completed and passed: window A's layout stays block-linear.
    int copied()
    {
        EXPECT_EQ(d.offerCopy(), 0);
        const int target = d.ring.drawing;
        EXPECT_EQ(d.completionFlip.state(), SwitchCompletionFlip::kArmed);
        d.complete(0);
        d.donePass();
        return target;
    }

    bool apertureWritten() const
    {
        for (const uint32_t word : d.model.written)
            if (word >= kApertureFirst && word <= kApertureLast)
                return true;
        return false;
    }
};

TEST_P(Refusing, TheFirstFrameEndLogsOnceAndTheGateFlipsTheFrame)
{
    const int target = copied();
    ASSERT_EQ(d.ring.pending, target);
    frame();
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_EQ(gLog[0], refusal(d.surfaceAddress(target), d.surfaceAddress(0)));
    EXPECT_EQ(d.model.assembly[Display::kWinbufStartAddr], d.surfaceAddress(target));
    EXPECT_EQ(d.model.requested, Display::kGeneralActReq | Display::kWinAActReq);
    EXPECT_EQ(d.ring.pending, target);
    EXPECT_EQ(d.ring.front, 0);
    EXPECT_TRUE(d.ring.replace) << "the ring's own fallback is not taken";
    frame();
    EXPECT_EQ(d.ring.front, target);
    EXPECT_EQ(d.activeEntry(), target);
    ASSERT_FALSE(d.shown.empty());
    EXPECT_EQ(d.shown.back().first, target);

    for (int i = 0; i < 6; ++i) {
        ASSERT_EQ(d.offerCopy(), 0);
        EXPECT_EQ(d.completionFlip.state(), SwitchCompletionFlip::kArmed) << "the next frame is armed as usual";
        const int next = d.ring.drawing;
        d.complete(0);
        d.donePass();
        frame();
        frame();
        EXPECT_EQ(d.ring.front, next);
        EXPECT_EQ(d.activeEntry(), next);
    }
    EXPECT_EQ(gLog.size(), 7u) << "each refusal is its own line";
}

// A completion flip that replaced a gate flip (the first offer's flip withdrawn, so it goes from the gate): the boundary
// took the gate flip, which is shown, and the completion's frame follows from the gate at the next boundary.
TEST_P(Refusing, AReplacedGateFlipIsShownFirst)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.withdrawCompletionFlip();
    EXPECT_EQ(d.completionFlip.state(), SwitchCompletionFlip::kIdle);
    d.complete(0);
    d.donePass();
    ASSERT_EQ(d.ring.pending, kSlot);
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    ASSERT_EQ(d.completionFlip.state(), SwitchCompletionFlip::kArmed);
    d.complete(0);
    d.donePass();
    ASSERT_EQ(d.ring.superseded, kSlot);
    frame();
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_EQ(gLog[0], refusal(d.surfaceAddress(kSlot + 1), d.surfaceAddress(kSlot)));
    ASSERT_FALSE(d.shown.empty());
    EXPECT_EQ(d.shown.back().first, kSlot);
    EXPECT_EQ(d.ring.front, kSlot);
    frame();
    EXPECT_EQ(d.ring.front, kSlot + 1);
    EXPECT_EQ(d.activeEntry(), kSlot + 1);
}

// Double-buffered already (recoverFlip ran before): the refusal still reads as a request out, never a panic.
TEST_P(Refusing, ARefusalAfterTheRingsFallbackDoesNotPanic)
{
    d.ring.replace = false;
    const int target = copied();
    EXPECT_NO_THROW(frame());
    EXPECT_EQ(gLog.size(), 1u);
    EXPECT_NO_THROW(frame());
    EXPECT_EQ(d.ring.front, target);
}

// The pass kicked the next offer, armed before the refusal: it is withdrawn and flips from the gate.
TEST_P(Refusing, AnOfferArmedBeforeTheRefusalIsWithdrawn)
{
    copied();
    ASSERT_EQ(d.offerCopy(), 0);
    const int next = d.ring.drawing;
    ASSERT_EQ(d.completionFlip.state(), SwitchCompletionFlip::kArmed);
    frame();
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_EQ(d.completionFlip.state(), SwitchCompletionFlip::kIdle);
    d.model.written.clear();
    d.complete(0);
    EXPECT_TRUE(d.model.written.empty());
    d.donePass();
    frame();
    EXPECT_EQ(d.ring.front, next);
    EXPECT_EQ(d.activeEntry(), next);
}

// The next completion lands inside the frame-end action, after the boundary: its request is still out at that latch
// read, so the verdict is the next frame end's, and there is one line.
TEST_P(Refusing, ACompletionInsideTheFrameEndActionIsJudgedAtTheNextFrameEnd)
{
    const int first = copied();
    ASSERT_EQ(d.offerCopy(), 0);
    const int next = d.ring.drawing;
    d.midTick = [this] {
        d.complete(0);
        d.midTick = nullptr;
    };
    frame();
    EXPECT_TRUE(gLog.empty());
    EXPECT_EQ(d.ring.pending, next);
    EXPECT_EQ(d.ring.superseded, first);
    frame();
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_EQ(gLog[0], refusal(d.surfaceAddress(next), d.surfaceAddress(0)));
    frame();
    EXPECT_EQ(d.ring.front, next);
    EXPECT_EQ(d.activeEntry(), next);
}

// In any order, every completion flip such a unit refuses is one line and that frame flips from the gate: the ring
// never reads a latch it cannot explain and presents go on. SC7 is left out: it takes the last request as shown, which
// such a unit would not have in its assembly copy.
TEST_P(Refusing, RandomOrdersRefuseEachFlipAndNeverMisreadALatch)
{
    Walk walk;
    walk.suspends = false;
    randomWalk(d, 200000, walk);
    EXPECT_GT(gLog.size(), 100u);
    EXPECT_GT(walk.completed, 5000u) << walk.completed;
    EXPECT_GT(d.shown.size(), 5000u) << d.shown.size();
}

INSTANTIATE_TEST_SUITE_P(Aperture, Refusing, testing::Values(DcModel::kDropsWinbuf, DcModel::kParksWinbuf));

class Apertures : public testing::Test {
protected:
    Display d;

    void SetUp() override
    {
        gOffer = nullptr;
        gLog.clear();
    }

    void frame()
    {
        d.boundary();
        d.tick();
    }
};

// Window A's apertures are a fact (TRM 24.10-24.11; display.completion_flip winbuf_alias), not something a latch has
// to prove first: the first completion flip may change window A's layout.
TEST_F(Apertures, TheFirstFlipMayChangeWindowAsLayout)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    EXPECT_EQ(d.completionFlip.state(), SwitchCompletionFlip::kArmed) << "a pitch slot after a block-linear front";
    d.complete(0);
    d.donePass();
    frame();
    EXPECT_EQ(d.ring.front, kSlot);
    EXPECT_EQ(d.activeEntry(), kSlot);
    ASSERT_EQ(d.offer(kSlot + 3), 0);
    EXPECT_EQ(d.completionFlip.state(), SwitchCompletionFlip::kArmed) << "a tiled slot after a pitch front";
    EXPECT_TRUE(gLog.empty());
}

}  // namespace
}  // namespace completion_flip_test
