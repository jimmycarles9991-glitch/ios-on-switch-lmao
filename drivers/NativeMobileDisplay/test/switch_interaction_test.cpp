// Host test of drivers/NativeMobileDisplay/SwitchInteraction.h: which presents a touch interaction window counts and
// the line it closes with.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "../SwitchInteraction.h"

namespace {

constexpr uint64_t kPeriod = 16667;

std::string closed(SwitchInteraction &w, uint64_t lastDown)
{
    char line[512];
    const size_t n = w.close(lastDown, line, sizeof(line));
    EXPECT_FALSE(w.isOpen());
    return std::string(line, n);
}

}  // namespace

TEST(SwitchInteraction, ActiveFromTheRunUntilTheTailAfterItsLastContact)
{
    EXPECT_FALSE(SwitchInteraction::active(5000000, 0, 0));
    EXPECT_FALSE(SwitchInteraction::active(999, 1000, 1000));
    EXPECT_TRUE(SwitchInteraction::active(1000, 1000, 1000));
    EXPECT_TRUE(SwitchInteraction::active(1000 + SwitchInteraction::kTailUs - 1, 1000, 1000));
    EXPECT_FALSE(SwitchInteraction::active(1000 + SwitchInteraction::kTailUs, 1000, 1000));
}

TEST(SwitchInteraction, CountsIntervalsBetweenItsOwnPresents)
{
    SwitchInteraction w;
    uint64_t t = 2000000;
    w.present(t, 3 * kPeriod, 3, 5, 1990000);  // answers the touch-down: its interval is the time before
    EXPECT_TRUE(w.isOpen());
    for (int i = 0; i < 50; i++)
        w.present(t += kPeriod, kPeriod, 1, 6, 1990000);
    w.present(t += 2 * kPeriod, 2 * kPeriod, 2, 5, 1990000);  // rate
    w.present(t += 3 * kPeriod, 3 * kPeriod, 3, 2, 1990000);  // gpu
    w.present(t += 5 * kPeriod, 5 * kPeriod, 5, 1, 1990000);  // ca
    w.present(t += 300000, 300000, 18, 0, 1990000);           // the screen went still for 300 ms
    EXPECT_EQ(closed(w, 2600000),
              "1310 ms (touch 610 ms), 55 presents: 1:50 2:1 3:1 4+:2, 24 periods lost, worst 300.0 ms, still 1; "
              "long idle 1 ca 1 gpu 1 queue 0 latch 0 rate 1");
}

TEST(SwitchInteraction, LongStallsRemainInTheDeliveryDenominator)
{
    SwitchInteraction w;
    w.present(1000000, 0, 0, 6, 1000000);
    w.present(2000000, 1000000, 60, 2, 1000000);
    EXPECT_EQ(closed(w, 1900000),
              "1000 ms (touch 900 ms), 2 presents: 1:0 2:0 3:0 4+:1, 59 periods lost, worst 1000.0 ms, still 1; "
              "long idle 0 ca 0 gpu 1 queue 0 latch 0 rate 0");
}

TEST(SwitchInteraction, ANewWindowStartsClean)
{
    SwitchInteraction w;
    w.present(1000000, 0, 0, 6, 1000000);
    w.present(1000000 + 2 * kPeriod, 2 * kPeriod, 2, 5, 1000000);
    closed(w, 1000000);
    w.present(9000000, 4 * kPeriod, 4, 5, 8990000);
    w.present(9000000 + kPeriod, kPeriod, 1, 6, 8990000);
    EXPECT_EQ(closed(w, 9000000),
              "26 ms (touch 10 ms), 2 presents: 1:1 2:0 3:0 4+:0, 0 periods lost, worst 16.6 ms, still 0; "
              "long idle 0 ca 0 gpu 0 queue 0 latch 0 rate 0");
}

TEST(SwitchInteraction, ALineCutShortIsTerminated)
{
    SwitchInteraction w;
    w.present(1000000, 0, 0, 6, 1000000);
    for (size_t size = 1; size < 40; size++) {
        char line[64];
        w.present(1000000, 0, 0, 6, 1000000);
        const size_t n = w.close(1000000, line, size);
        EXPECT_LT(n, size);
        EXPECT_EQ(line[n], 0);
    }
}
