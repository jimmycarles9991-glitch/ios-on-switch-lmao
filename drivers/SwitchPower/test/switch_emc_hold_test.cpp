// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchEMCHold.h played out as SwitchEMC.cpp runs it, over a clock in 100 us steps: frame lines at 60 Hz, the frame
// core's start edge (a hint that wakes the EMC's work loop), CPU work, and the CPU governor's 8 ms tick with the CPU time
// of its window. Counts what a burst of frames or of CPU work costs the EMC's loop: its wake-ups.
#include "drivers/SwitchPower/SwitchEMCHold.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {
constexpr uint64_t kUs = 1;
constexpr uint64_t kMs = 1000 * kUs;
constexpr uint64_t kStep = 100 * kUs;
constexpr uint64_t kFrameHold = 100 * kMs;  // SwitchEMC.cpp frame_ticks
constexpr uint64_t kHintHold = 500 * kMs;   // SWITCH_EMC_HINT_HOLD_US
constexpr uint64_t kBusyFloor = 700 * kUs;  // SWITCH_EMC_BUSY_FLOOR_US
constexpr uint64_t kTick = 8 * kMs;         // the CPU governor's window
constexpr uint64_t kLoopCost = 60 * kUs;    // one pass of the EMC's work loop, on some CPU

struct Loop {
    switch_emc_hold hold = {kFrameHold, kBusyFloor, 0, 0, 0, 0};
    bool governed = true;
    uint64_t hintUntil = 0, lastFrame = 0;
    uint64_t window = 0;  // CPU time in the tick's window so far
    bool keep = false;
    unsigned wakes = 0, drops = 0;
    uint64_t released = 0;  // when the hold last went off

    void program(uint64_t now)
    {
        ++wakes;
        window += kLoopCost;
        const bool kept = switch_emc_hold_program(&hold, now, hintUntil, lastFrame, governed);
        if (keep && !kept) {
            released = now;
            ++drops;
        }
        keep = kept;
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
        if (governed && now % kTick == 0) {
            const uint64_t busy = window;
            window = 0;
            if (switch_emc_hold_tick(&hold, now, busy, hintUntil, lastFrame, keep))
                program(now);
        }
    }

    // Frames every 16.7 ms from `from` for `ms`, then quiet until `until`.
    void burst(uint64_t from, uint64_t ms, uint64_t until)
    {
        uint64_t next = from;
        for (uint64_t now = from; now < until; now += kStep) {
            if (now < from + ms && now >= next)
                frameLine(now);
            while (next <= now)
                next += 16700 * kUs;
            at(now);
        }
    }

    // `perTick` of CPU time in every window that ends before `stop`, nothing after it, until `until`.
    void work(uint64_t from, uint64_t stop, uint64_t perTick, uint64_t until)
    {
        for (uint64_t now = from; now < until; now += kStep) {
            if (now < stop && now % kTick == 0)
                window += perTick;
            at(now);
        }
    }
};
}  // namespace

// Two seconds of frames: the start edge wakes the loop, the governor's tick reports the end.
TEST(SwitchEMCHold, ABurstOfFramesIsTwoWakes)
{
    Loop loop;
    loop.burst(1000 * kMs, 2000 * kMs, 4000 * kMs);
    EXPECT_EQ(loop.wakes, 2u) << "loop wake-ups for one burst";
    EXPECT_FALSE(loop.keep);
    EXPECT_LE(loop.released, loop.lastFrame + kFrameHold + kTick) << "the hold ends within a tick of the frames";
    EXPECT_GE(loop.released, loop.lastFrame + kFrameHold);
}

TEST(SwitchEMCHold, EachBurstCostsTheSame)
{
    Loop loop;
    for (uint64_t burst = 0; burst < 5; ++burst)
        loop.burst((1000 + burst * 4000) * kMs, 1500 * kMs, (1000 + burst * 4000 + 3000) * kMs);
    EXPECT_EQ(loop.wakes, 10u);
}

// A touch alone holds for kHintHold: one wake to hold, one to release.
TEST(SwitchEMCHold, AHintAloneHoldsForItsDeadline)
{
    Loop loop;
    loop.hint(1000 * kMs);
    loop.work(1000 * kMs, 0, 0, 2000 * kMs);
    EXPECT_EQ(loop.wakes, 2u);
    EXPECT_FALSE(loop.keep);
    EXPECT_LE(loop.released, 1000 * kMs + kHintHold + kTick);
}

// Without the governor's tick (the PLL ladder, or the governor stopped) nothing accounts for the CPUs' work: the hold
// stays on after the frames, whatever runs.
TEST(SwitchEMCHold, WithoutTheTickTheHoldStays)
{
    Loop loop;
    loop.governed = false;
    loop.burst(1000 * kMs, 2000 * kMs, 4000 * kMs);
    EXPECT_TRUE(loop.keep);
    EXPECT_EQ(loop.drops, 0u);
    EXPECT_TRUE(switch_emc_hold_program(&loop.hold, 9000 * kMs, 0, loop.lastFrame, false));
}

// The governor starts again: its retick programs the hold, which ends there if its deadlines have passed.
TEST(SwitchEMCHold, TheRetickEndsAHoldWhoseDeadlinesPassed)
{
    Loop loop;
    loop.governed = false;
    loop.hint(1000 * kMs);
    loop.work(1000 * kMs, 0, 0, 3000 * kMs);
    ASSERT_TRUE(loop.keep);
    loop.governed = true;
    loop.program(3000 * kMs);
    loop.work(3000 * kMs, 0, 0, 3100 * kMs);
    EXPECT_FALSE(loop.keep);
    EXPECT_LE(loop.released, 3000 * kMs + kTick);
}

// The tick read the stamp just before a frame landed: the loop finds the hold still on, starts a new generation, and
// the tick reports that one's end in turn.
TEST(SwitchEMCHold, AnEndTheLoopFindsWasNotOneIsReportedAgain)
{
    switch_emc_hold hold = {kFrameHold, kBusyFloor, 0, 0, 0, 0};
    ASSERT_TRUE(switch_emc_hold_program(&hold, 1000 * kMs, 0, 1000 * kMs, true));
    EXPECT_TRUE(switch_emc_hold_tick(&hold, 1104 * kMs, 0, 0, 1000 * kMs, true));
    EXPECT_FALSE(switch_emc_hold_tick(&hold, 1112 * kMs, 0, 0, 1000 * kMs, true)) << "one wake per end";
    EXPECT_TRUE(switch_emc_hold_program(&hold, 1105 * kMs, 0, 1101 * kMs, true));
    EXPECT_FALSE(switch_emc_hold_tick(&hold, 1112 * kMs, 0, 0, 1101 * kMs, true));
    EXPECT_TRUE(switch_emc_hold_tick(&hold, 1208 * kMs, 0, 0, 1101 * kMs, true));
    EXPECT_FALSE(switch_emc_hold_program(&hold, 1209 * kMs, 0, 1101 * kMs, true));
    EXPECT_FALSE(switch_emc_hold_tick(&hold, 1216 * kMs, 0, 0, 1101 * kMs, true));
}

// An app's launch as boot 2 measured it: CPUs busy, nothing drawn, no hint after the first half second. The first
// window of work raises (one wake), the top rate holds through every window of work without another wake, and the
// hold ends a frame hold after the last one.
TEST(SwitchEMCHold, CpuWorkHoldsTheTopWithoutFramesOrHints)
{
    Loop loop;
    loop.work(1000 * kMs, 4000 * kMs, 2700 * kUs, 5000 * kMs);
    EXPECT_EQ(loop.wakes, 2u) << "one wake to hold, one to release";
    EXPECT_EQ(loop.drops, 1u) << "the hold went off during the work";
    EXPECT_GE(loop.released, 4000 * kMs - kTick + kFrameHold);
    EXPECT_LE(loop.released, 4000 * kMs + kFrameHold + kTick);
}

// The work starts while the EMC runs below its top rate: the tick that ends its first window wakes the loop.
TEST(SwitchEMCHold, TheFirstWindowOfWorkRaises)
{
    Loop loop;
    loop.work(1000 * kMs, 1000 * kMs + 5 * kMs, 2 * kMs, 1000 * kMs + kTick + kStep);
    EXPECT_EQ(loop.wakes, 1u);
    EXPECT_TRUE(loop.keep);
}

// After a hint, only the loop's own passes and a daemon's 200 us a window run: the hold ends at the hint's deadline
// and stays off; the loop's pass at the end does not hold the top rate again.
TEST(SwitchEMCHold, TheLoopsOwnPassesDoNotHoldTheTop)
{
    Loop loop;
    loop.hint(1000 * kMs);
    loop.work(1000 * kMs, 4000 * kMs, 200 * kUs, 4000 * kMs);
    EXPECT_FALSE(loop.keep);
    EXPECT_EQ(loop.wakes, 2u);
    EXPECT_LE(loop.released, 1000 * kMs + kHintHold + kTick);
}
