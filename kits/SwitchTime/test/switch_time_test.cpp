// kits/SwitchTime's unit conversions over a 19.2 MHz model clock.
#include "kits/SwitchTime/SwitchTime.h"

#include <gtest/gtest.h>

uint64_t gSwitchTimeNow;

namespace {
TEST(SwitchTime, Conversions)
{
    EXPECT_EQ(switch_abs_from_ms(1), 19200u);
    EXPECT_EQ(switch_abs_from_us(10), 192u);
    EXPECT_EQ(switch_abs_to_us(19200), 1000u);
    EXPECT_EQ(switch_abs_to_ms(19200 * 250), 250u);
}

TEST(SwitchTime, ElapsedAndDeadlines)
{
    gSwitchTimeNow = 1'000'000;
    const uint64_t start = gSwitchTimeNow;
    gSwitchTimeNow += 19200 * 3;
    EXPECT_EQ(switch_ms_since(start), 3u);
    EXPECT_EQ(switch_us_since(start), 3000u);
    EXPECT_EQ(switch_deadline_us(100), gSwitchTimeNow + 1920);
    EXPECT_EQ(switch_deadline_ms(2), gSwitchTimeNow + 38400);
    EXPECT_EQ(switch_now_us(), gSwitchTimeNow * 625 / 12 / 1000);
    EXPECT_EQ(switch_now_ns(), gSwitchTimeNow * 625 / 12);
    EXPECT_EQ(switch_now_ms(), gSwitchTimeNow * 625 / 12 / 1000000);
    EXPECT_EQ(switch_ns_since(start), 3000000u);
}
}  // namespace
