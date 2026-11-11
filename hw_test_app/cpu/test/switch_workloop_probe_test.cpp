extern "C" {
#include "hw_test_app/cpu/switch_workloop_probe.h"
}

#include <gtest/gtest.h>

TEST(WorkloopProbe, EveryWaiterLeavesOnlyWhenHandedTheWorkloop)
{
    switch_workloop_result r{};
    ASSERT_TRUE(switch_workloop_probe(4, 8, 200, false, &r));
    EXPECT_EQ(r.handoffs, 4u * 8u * 200u);
    EXPECT_EQ(r.early, 0u);
    EXPECT_EQ(r.errors, 0u) << "first errno " << r.first_error;
    EXPECT_EQ(r.stuck, 0u);
    EXPECT_GT(r.parked, r.handoffs / 2) << "the waits must block in the kernel, not find their wake posted";
}

TEST(WorkloopProbe, AKeptKnoteReturnsEveryLaterWaitAtOnce)
{
    switch_workloop_result r{};
    ASSERT_TRUE(switch_workloop_probe(1, 4, 50, true, &r));
    EXPECT_GT(r.early, 0u);
    EXPECT_EQ(r.stuck, 0u);
}

TEST(WorkloopProbe, RefusesWhatItCannotRun)
{
    switch_workloop_result r{};
    EXPECT_FALSE(switch_workloop_probe(1, 0, 1, false, &r));
    EXPECT_FALSE(switch_workloop_probe(1, 17, 1, false, &r));
    EXPECT_FALSE(switch_workloop_probe(1, 1, 0, false, &r));
}
