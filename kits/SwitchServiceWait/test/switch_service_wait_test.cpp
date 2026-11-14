// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <chrono>
#include "../SwitchServiceWait.h"

namespace {
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

TEST(SwitchServiceWait, APublishedServiceEndsTheWaitAtOnce)
{
    const auto start = Clock::now();
    EXPECT_TRUE(switch_service_wait("IOResources", "SwitchServiceWaitNoSuchResource", 5000));
    EXPECT_LT(msSince(start), 1000.0);
}

// IOKit publishes its own "IOKit" resource at start: a driver's "absent" resource ends the wait as soon as it is there.
TEST(SwitchServiceWait, APublishedAbsentResourceEndsTheWaitAtOnce)
{
    const auto start = Clock::now();
    EXPECT_FALSE(switch_service_wait("SwitchServiceWaitNoSuchService", "IOKit", 5000));
    EXPECT_LT(msSince(start), 1000.0);
}

TEST(SwitchServiceWait, NeitherWaitsOutTheBound)
{
    const auto start = Clock::now();
    EXPECT_FALSE(switch_service_wait("SwitchServiceWaitNoSuchService", "SwitchServiceWaitNoSuchResource", 300));
    EXPECT_GE(msSince(start), 290.0);
    EXPECT_LT(msSince(start), 3000.0);
}
}  // namespace
