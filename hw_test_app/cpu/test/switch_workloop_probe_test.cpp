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
    // A waker can post its wake before its waiter's wait begins, which is correct, so how many waits block in the
    // kernel is the scheduler's: a loaded host parks few of them
    EXPECT_GT(r.parked, 0u) << "the waits must block in the kernel, not always find their wake posted";
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
