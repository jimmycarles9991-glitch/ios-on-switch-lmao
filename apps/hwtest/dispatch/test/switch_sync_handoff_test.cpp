extern "C" {
#include "apps/hwtest/dispatch/switch_sync_handoff.h"
}

#include <gtest/gtest.h>

#include <string>

namespace {
switch_sync_result RunBroken(switch_sync_break inject, uint32_t deadline_ms)
{
    const switch_sync_config config{8, 1000, 4, 500, 4, deadline_ms, inject};
    switch_sync_result r{};
    EXPECT_TRUE(switch_sync_handoff(&config, &r));
    return r;
}
}  // namespace

TEST(SyncHandoff, AHealthyLibdispatchPassesEveryCheck)
{
    const switch_sync_config config{16, 4000, 4, 2000, 16, 120000, SWITCH_SYNC_BREAK_NONE};
    switch_sync_result r{};
    ASSERT_TRUE(switch_sync_handoff(&config, &r));
    EXPECT_TRUE(r.finished) << r.wedge;
    EXPECT_EQ(r.overlaps, 0u) << r.first_overlap;
    EXPECT_EQ(r.counter_mismatches, 0u);
    EXPECT_EQ(r.async_sent, 4u * 2000u);
    EXPECT_EQ(r.async_received, r.async_sent);
    EXPECT_EQ(r.duplicates, 0u);
    EXPECT_EQ(r.out_of_order, 0u);
    EXPECT_EQ(r.activity_mismatches, 0u);
    EXPECT_EQ(r.activities, 16u + 4u);
    EXPECT_EQ(r.faults, 0u);
    EXPECT_EQ(r.parked, 0u);
    EXPECT_GT(r.syncs, 16u * 4000u);
    EXPECT_GT(r.nested, 0u);
    EXPECT_GT(r.barriers, 0u);
    EXPECT_GT(r.async_and_waits, 0u);
    EXPECT_GT(r.servicer_syncs, 0u);
    EXPECT_EQ(r.inactive_waiters, 8u);
    EXPECT_EQ(r.retarget_rounds, 16u);
    EXPECT_GT(r.retarget_crossed, 0u) << "parked rounds " << r.retarget_parked;
}

TEST(SyncHandoff, AThreadEnteringItsQueueTwiceIsAnOverlapAndParksThere)
{
    const switch_sync_result r = RunBroken(SWITCH_SYNC_BREAK_OVERLAP, 60000);
    EXPECT_FALSE(r.finished);
    EXPECT_EQ(r.overlaps, 1u);
    EXPECT_EQ(r.parked, 1u);
    EXPECT_NE(std::string(r.first_overlap), "");
    EXPECT_NE(std::string(r.wedge).find("parked 1"), std::string::npos) << r.wedge;
}

TEST(SyncHandoff, ABlockThatNeverReturnsIsAWedgeNamingItsQueueAndThreads)
{
    const switch_sync_result r = RunBroken(SWITCH_SYNC_BREAK_WEDGE, 3000);
    EXPECT_FALSE(r.finished);
    EXPECT_EQ(r.parked, 0u);
    const std::string wedge = r.wedge;
    EXPECT_NE(wedge.find(" owner 0x"), std::string::npos) << wedge;
    EXPECT_NE(wedge.find(" in 0x"), std::string::npos) << wedge;
}

TEST(SyncHandoff, AnItemUnderAnotherActivityIsAMismatch)
{
    const switch_sync_result r = RunBroken(SWITCH_SYNC_BREAK_ACTIVITY, 60000);
    EXPECT_TRUE(r.finished) << r.wedge;
    // The item's block, and its servicer sync's block when it makes one.
    EXPECT_GE(r.activity_mismatches, 1u);
    EXPECT_LE(r.activity_mismatches, 2u);
}

TEST(SyncHandoff, AnItemEnqueuedTwiceIsADuplicateOutOfOrder)
{
    const switch_sync_result r = RunBroken(SWITCH_SYNC_BREAK_DUPLICATE, 60000);
    EXPECT_TRUE(r.finished) << r.wedge;
    EXPECT_EQ(r.duplicates, 1u);
    EXPECT_EQ(r.out_of_order, 1u);
    EXPECT_EQ(r.async_received, r.async_sent);
}

TEST(SyncHandoff, RefusesWhatItCannotRun)
{
    switch_sync_result r{};
    const switch_sync_config no_threads{0, 1, 1, 1, 1, 1000, SWITCH_SYNC_BREAK_NONE};
    const switch_sync_config too_many{17, 1, 1, 1, 1, 1000, SWITCH_SYNC_BREAK_NONE};
    const switch_sync_config no_rounds{1, 1, 1, 1, 0, 1000, SWITCH_SYNC_BREAK_NONE};
    EXPECT_FALSE(switch_sync_handoff(&no_threads, &r));
    EXPECT_FALSE(switch_sync_handoff(&too_many, &r));
    EXPECT_FALSE(switch_sync_handoff(&no_rounds, &r));
}
