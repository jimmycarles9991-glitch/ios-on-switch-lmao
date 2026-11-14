// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchEMCHold.h played out as SwitchEMC.cpp runs it, over a millisecond clock: frame lines at 60 Hz, the frame core's
// start edge (a hint that wakes the EMC's work loop), the hold timer, and the CPU governor's 8 ms tick. Counts what a
// burst of frames costs the EMC's loop: hold-timer arms and loop wake-ups.
#include "drivers/SwitchPower/SwitchEMCHold.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {
constexpr uint64_t kMs = 1;
constexpr uint64_t kFrameHold = 100 * kMs;  // SwitchEMC.cpp frameTicks
constexpr uint64_t kHintHold = 500 * kMs;   // SWITCH_EMC_HINT_HOLD_US
constexpr uint64_t kTickMs = 8;             // the CPU governor's window

struct Loop {
    switch_emc_hold hold = {kFrameHold, 0, 0, 0};
    bool governed = true;
    uint64_t hintUntil = 0, lastFrame = 0, timerAt = 0;
    bool keep = false;
    unsigned arms = 0, wakes = 0;
    uint64_t released = 0;  // when the hold last went off

    void program(uint64_t now)
    {
        ++wakes;
        const switch_emc_hold_step step = switch_emc_hold_program(&hold, now, hintUntil, lastFrame, governed);
        if (keep && !step.keep)
            released = now;
        keep = step.keep;
        if (step.arm_at) {
            timerAt = step.arm_at;
            ++arms;
        }
    }

    // A hint (or the frame core's start edge): the top rate for kHintHold, and the loop woken unless already held.
    void hint(uint64_t now)
    {
        if (now + kHintHold > hintUntil)
            hintUntil = now + kHintHold;
        if (!keep)
            program(now);
    }

    void frameLine(uint64_t now)
    {
        const bool started = !lastFrame || now - lastFrame >= kFrameHold;
        lastFrame = now;
        if (started)
            hint(now);
    }

    void at(uint64_t now)
    {
        if (timerAt && now >= timerAt) {
            timerAt = 0;
            program(now);
        }
        if (governed && now % kTickMs == 0 && switch_emc_hold_tick(&hold, now, hintUntil, lastFrame))
            program(now);
    }

    // Frames every 16.7 ms from `from` for `ms`, then quiet until `until`.
    void burst(uint64_t from, uint64_t ms, uint64_t until)
    {
        uint64_t next = from;
        for (uint64_t now = from; now < until; ++now) {
            if (now < from + ms && now >= next) {
                frameLine(now);
                next += 50 * kMs / 3 + (next % 3 == 2 ? 1 : 0);
            }
            at(now);
        }
    }
};
}  // namespace

// Two seconds of frames: the start edge wakes the loop, the governor's tick reports the end; the hold timer is never
// armed while the tick runs.
TEST(SwitchEMCHold, ABurstOfFramesIsTwoWakesAndNoTimerArm)
{
    Loop loop;
    loop.burst(1000, 2000, 4000);
    EXPECT_EQ(loop.arms, 0u) << "hold-timer arms for one burst";
    EXPECT_EQ(loop.wakes, 2u) << "loop wake-ups for one burst";
    EXPECT_FALSE(loop.keep);
    EXPECT_LE(loop.released, loop.lastFrame + kFrameHold + kTickMs) << "the hold ends within a tick of the frames";
    EXPECT_GE(loop.released, loop.lastFrame + kFrameHold);
}

TEST(SwitchEMCHold, EachBurstCostsTheSame)
{
    Loop loop;
    for (uint64_t burst = 0; burst < 5; ++burst)
        loop.burst(1000 + burst * 4000, 1500, 1000 + burst * 4000 + 3000);
    EXPECT_EQ(loop.arms, 0u);
    EXPECT_EQ(loop.wakes, 10u);
}

// A touch alone holds for kHintHold: one wake to hold, one to release.
TEST(SwitchEMCHold, AHintAloneHoldsForItsDeadline)
{
    Loop loop;
    loop.hint(1000);
    for (uint64_t now = 1000; now < 2000; ++now)
        loop.at(now);
    EXPECT_EQ(loop.wakes, 2u);
    EXPECT_FALSE(loop.keep);
    EXPECT_LE(loop.released, 1000 + kHintHold + kTickMs);
}

// Without the governor's tick (the PLL ladder, or the governor stopped) the loop's own timer ends the hold.
TEST(SwitchEMCHold, WithoutTheTickTheTimerEndsTheHold)
{
    Loop loop;
    loop.governed = false;
    loop.burst(1000, 2000, 4000);
    EXPECT_FALSE(loop.keep);
    EXPECT_GE(loop.released, loop.lastFrame + kFrameHold);
    EXPECT_LE(loop.released, loop.lastFrame + kFrameHold + kMs);
}

// The tick read the stamp just before a frame landed: the loop finds the hold still on, starts a new generation, and
// the tick reports that one's end in turn.
TEST(SwitchEMCHold, AnEndTheLoopFindsWasNotOneIsReportedAgain)
{
    switch_emc_hold hold = {kFrameHold, 0, 0, 0};
    ASSERT_TRUE(switch_emc_hold_program(&hold, 1000, 0, 1000, true).keep);
    EXPECT_TRUE(switch_emc_hold_tick(&hold, 1104, 0, 1000));
    EXPECT_FALSE(switch_emc_hold_tick(&hold, 1112, 0, 1000)) << "one wake per end";
    EXPECT_TRUE(switch_emc_hold_program(&hold, 1105, 0, 1101, true).keep);
    EXPECT_FALSE(switch_emc_hold_tick(&hold, 1112, 0, 1101));
    EXPECT_TRUE(switch_emc_hold_tick(&hold, 1208, 0, 1101));
    EXPECT_FALSE(switch_emc_hold_program(&hold, 1209, 0, 1101, true).keep);
    EXPECT_FALSE(switch_emc_hold_tick(&hold, 1216, 0, 1101));
}
