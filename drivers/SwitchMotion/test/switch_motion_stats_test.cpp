// SwitchMotionStats.h on the host: the motion driver's 10 s window, counted from its interrupt actions, and its line.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "drivers/SwitchMotion/SwitchMotionStats.h"

namespace {

constexpr uint64_t kSecond = 1000000000;  // ticks are nanoseconds
constexpr uint64_t kWindow = 10 * kSecond, kGap = 2 * kSecond;

SwitchMotionState detector(uint32_t drains, uint32_t records)
{
    return {25000, {true, false}, {0, 0}, {0, 0}, true, false, true, 5, drains, records, 0};
}

TEST(SwitchMotionStats, AWindowEndsAfterTenSecondsOfInterrupts)
{
    SwitchMotionStats s;
    uint64_t now = kSecond;
    unsigned due = 0;
    for (unsigned i = 0; i < 60; ++i, now += kSecond / 5)
        due += switchMotionInterrupt(s, now, kWindow, kGap, detector(i, i * 5)) ? 1 : 0;
    EXPECT_EQ(due, 10u) << "every action from 10 s on, until the caller restarts the window";
    EXPECT_EQ(s.drains, 0u) << "the driver's counts as the window began";
}

TEST(SwitchMotionStats, AGapRestartsTheWindow)
{
    SwitchMotionStats s;
    switchMotionInterrupt(s, kSecond, kWindow, kGap, detector(0, 0));
    s.posted[0] = 7;
    EXPECT_FALSE(switchMotionInterrupt(s, 12 * kSecond, kWindow, kGap, detector(3, 15)))
        << "converting stopped for 11 s: not a 10 s window";
    EXPECT_EQ(s.start, 12 * kSecond);
    EXPECT_EQ(s.interrupts, 1u);
    EXPECT_EQ(s.posted[0], 0u);
    EXPECT_EQ(s.drains, 3u);
}

TEST(SwitchMotionStats, TheLineSaysWhatItConvertsForWhomAndWhatWokeIt)
{
    SwitchMotionStats s;
    switchMotionRestart(s, kSecond, detector(100, 500));
    s.interrupts = 50;
    s.empty = 2;
    s.posted[0] = 0;
    SwitchMotionState state = detector(150, 750);
    state.intervalUS[1] = 10000;
    state.batchUS[1] = 15000;
    char line[400];
    switchMotionLine(line, sizeof(line), s, state, 10000);
    EXPECT_EQ(std::string(line),
              "10000 ms at 25000 mHz: accel 1 (client 0 us, batch 0 us), gyro 0 (client 10000 us, batch 15000 us), "
              "detector 1, panel lit 0, FIFO 1 (watermark 5); 50 interrupts (5/s), 2 empty, posted 0 accel 0 gyro, "
              "50 drains of 250 records, 0 overflows");
}

TEST(SwitchMotionStats, ALineCutShortStaysInItsBuffer)
{
    SwitchMotionStats s;
    char line[32];
    EXPECT_EQ(switchMotionLine(line, sizeof(line), s, detector(0, 0), 10000), sizeof(line) - 1);
    EXPECT_EQ(std::string(line).size(), sizeof(line) - 1);
}

}  // namespace
