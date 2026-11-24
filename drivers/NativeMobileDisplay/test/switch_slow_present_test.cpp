// Host test of drivers/NativeMobileDisplay/SwitchSlowPresent.h: the cause a present two or more refresh periods after
// the one before it is charged to.
#include <gtest/gtest.h>

#include "../SwitchSlowPresent.h"

namespace {

using switch_slow_present::cause;
using switch_slow_present::interval;
using switch_slow_present::kCa;
using switch_slow_present::kGpu;
using switch_slow_present::kIdle;
using switch_slow_present::kLatch;
using switch_slow_present::kMasked;
using switch_slow_present::kQueue;
using switch_slow_present::kRate;

constexpr uint64_t kPeriod = 16667;
constexpr uint64_t kLast = 1000000;  // the previous present
constexpr uint64_t kDue = kLast + kPeriod;

}  // namespace

// A swap after the boundary is the compositor's lateness only when the frame end it answered was seen: with FRAME_END
// masked before it (the display idle), late and nothing-to-draw look the same, and the cause says so.
TEST(SwitchSlowPresent, ALateSwapWithNoFrameEndSeenIsMaskedNotCa)
{
    EXPECT_EQ(cause({kDue + 9000, kDue + 9100, kDue + 15000, 0}, kLast, kPeriod, false), kMasked);
    EXPECT_EQ(cause({kDue + 9000, kDue + 9100, kDue + 15000, kDue}, kLast, kPeriod, false), kCa);
    EXPECT_EQ(cause({kDue + 2000, kDue + 2100, kDue + 15000, kDue}, kLast, kPeriod, false), kRate);
    EXPECT_EQ(cause({kDue + 9000, kDue + 9100, kDue + 15000, 0}, kLast, kPeriod, true), kIdle);
    EXPECT_EQ(cause({0, 0, kDue + 15000, 0}, kLast, kPeriod, false), kCa) << "nothing known of its swap";
}

TEST(SwitchSlowPresent, ASwapInTimeIsChargedToWhatCameAfterIt)
{
    EXPECT_EQ(cause({kLast + 9000, kLast + 9100, kDue + 5000, kLast}, kLast, kPeriod, false), kGpu);
    EXPECT_EQ(cause({kLast + 9000, kLast + 16000, kDue + 5000, kLast}, kLast, kPeriod, false), kQueue);
    EXPECT_EQ(cause({kLast + 9000, kLast + 9100, kLast + 16000, kLast}, kLast, kPeriod, false), kLatch);
}

// A gap of 250 ms or more is a still screen outside a touch window; inside one it is timed like any slow present, in
// whole periods, since it may be the stall the touch waits on.
TEST(SwitchSlowPresent, AGapInsideATouchWindowIsASlowPresentInWholePeriods)
{
    const auto touched = interval(2683000, kPeriod, true);
    EXPECT_EQ(touched.periods, 161u);
    EXPECT_TRUE(touched.slow);
    const auto still = interval(2683000, kPeriod, false);
    EXPECT_EQ(still.periods, 161u);
    EXPECT_FALSE(still.slow);
    EXPECT_TRUE(interval(2 * kPeriod, kPeriod, false).slow);
    EXPECT_FALSE(interval(kPeriod, kPeriod, true).slow);
}
