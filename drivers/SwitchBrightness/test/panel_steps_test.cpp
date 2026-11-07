// TegraPanelSteps.h on the Mac: a DCS command walked through frame ends and step-timer expiries as TegraPanel.cpp
// drives it, against hekate's display_dsi_vblank_write order (enable, packet, an extra frame, then 14 us, reset 300 us,
// release 300 us, plain video).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../TegraPanelSteps.h"

namespace {
using namespace tegra_panel;

struct Walk {
    std::vector<std::string> events;
    uint32_t timerUS = 0;
    unsigned frameEnds = 0;
};

// Frame ends and timer expiries in the order the hardware gives them: a pending timer always expires before the next
// frame end (its waits are microseconds, a frame is 16.7 ms). `blanking` answers each blank check in turn.
Walk run(unsigned words, std::vector<bool> blanking)
{
    Walk w;
    Step step = kQueued;
    unsigned deferrals = 0;
    size_t check = 0;
    for (int guard = 0; step != kDone && guard < 64; ++guard) {
        Move move{};
        if (movesAtTimer(step)) {
            const bool inBlanking = step == kBlankCheck ? blanking.at(check++) : false;
            move = atTimer(step, inBlanking, deferrals);
            if (step == kBlankCheck && move.next == kSettled)
                ++deferrals;
            w.events.push_back("timer->" + std::to_string(move.next));
        } else {
            move = atFrameEnd(step, words);
            ++w.frameEnds;
            w.events.push_back("frame->" + std::to_string(move.next));
        }
        w.timerUS += move.timerUS;
        step = move.next;
    }
    EXPECT_EQ(step, kDone);
    return w;
}

TEST(PanelSteps, PacketTakesFourFrameEndsThenTheTimedReset)
{
    const Walk w = run(2, {true});
    EXPECT_EQ(w.frameEnds, 4u);
    EXPECT_EQ(w.timerUS, kBlankCheckUS + 2 * kResetSettleUS);
    const std::vector<std::string> expected = {
        "frame->" + std::to_string(kEnabled),   "frame->" + std::to_string(kWritten),
        "frame->" + std::to_string(kSettled),   "frame->" + std::to_string(kBlankCheck),
        "timer->" + std::to_string(kResetHeld), "timer->" + std::to_string(kResetReleased),
        "timer->" + std::to_string(kDone),
    };
    EXPECT_EQ(w.events, expected);
}

TEST(PanelSteps, NoPacketIsDoneAtTheFirstFrameEnd)
{
    const Walk w = run(0, {});
    EXPECT_EQ(w.frameEnds, 1u);
    EXPECT_EQ(w.timerUS, 0u);
}

TEST(PanelSteps, OutOfBlankingWaitsForTheNextFrameEndAtMostTwice)
{
    const Walk once = run(1, {false, true});
    EXPECT_EQ(once.frameEnds, 5u);
    const Walk never = run(1, {false, false, false});
    EXPECT_EQ(never.frameEnds, 4u + kResetDeferrals);
}

TEST(PanelSteps, FrameEndsLeaveTimerStepsAlone)
{
    for (Step s : {kBlankCheck, kResetHeld, kResetReleased, kIdle, kDone}) {
        const Move m = atFrameEnd(s, 2);
        EXPECT_EQ(m.next, s);
        EXPECT_EQ(m.timerUS, 0u);
    }
}

TEST(PanelSteps, TimersLeaveFrameStepsAlone)
{
    for (Step s : {kIdle, kQueued, kEnabled, kWritten, kSettled, kDone}) {
        EXPECT_FALSE(movesAtTimer(s));
        EXPECT_EQ(atTimer(s, true, 0).next, s);
    }
}

TEST(PanelSteps, TheResetHoldsAtLeastHekatesSettleTimes)
{
    EXPECT_EQ(atTimer(kBlankCheck, true, 0).timerUS, 300u);
    EXPECT_EQ(atTimer(kResetHeld, true, 0).timerUS, 300u);
    EXPECT_EQ(atFrameEnd(kSettled, 1).timerUS, 14u);
}
}  // namespace
