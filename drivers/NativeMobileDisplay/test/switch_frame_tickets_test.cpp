// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <type_traits>
#include "../SwitchFrameTickets.h"

#if SWITCH_FRAME_TIMELINE
TEST(SwitchFrameTickets, ActualSwapIdentitySurvivesGpuSubmissionAndLatch)
{
    SwitchFrameTickets tickets;
    int request = 0;
    const uint64_t id = tickets.accept(&request, 47, 2000000, 1000000);
    ASSERT_NE(id, 0u);
    EXPECT_EQ(tickets.accept(&request, 99, 3, 4), id);
    tickets.take(&request, 9);
    tickets.gpu(9, 13);
    const auto shown = tickets.shown(9, 2000010);
    EXPECT_EQ(shown.id, id);
    EXPECT_EQ(shown.swapId, 47u);
    EXPECT_EQ(shown.take, 9u);
    EXPECT_EQ(shown.gpu, 13u);
    EXPECT_EQ(shown.requested, 2000000u);
    EXPECT_EQ(shown.origin, 0u);
    EXPECT_EQ(tickets.totals.accepted, 1u);
    EXPECT_EQ(tickets.totals.shown, 1u);
    EXPECT_EQ(tickets.totals.unknownOrigin, 1u);
    EXPECT_EQ(tickets.totals.missedDeadline, 1u);
    tickets.shown(9, 3000000);
    EXPECT_EQ(tickets.totals.shown, 1u);
    tickets.retire(&request);
    EXPECT_EQ(tickets.find(&request), nullptr);
    EXPECT_EQ(tickets.totals.unshown, 0u);
    tickets.shown(9, 4000000);
    EXPECT_EQ(tickets.totals.shown, 1u);
    EXPECT_EQ(tickets.totals.unknownOrigin, 1u);
}

TEST(SwitchFrameTickets, ReplacedFramesNeverInheritTheNewFramesGpuOrPresentation)
{
    SwitchFrameTickets tickets;
    int first = 0, second = 0;
    tickets.accept(&first, 0xffffffffu, 100, 20);
    tickets.accept(&second, 0, 200, 30);
    tickets.take(&first, 1);
    tickets.take(&second, 2);
    tickets.gpu(1, 7);
    tickets.gpu(2, 8);
    const auto shown = tickets.shown(2, 200);
    EXPECT_EQ(shown.swapId, 0u);
    EXPECT_EQ(shown.gpu, 8u);
    tickets.retire(&first);
    tickets.retire(&second);
    EXPECT_EQ(tickets.totals.unshown, 1u);
    EXPECT_EQ(tickets.totals.shown, 1u);
    EXPECT_EQ(tickets.totals.missedDeadline, 0u);
}

TEST(SwitchFrameTickets, MissingOriginsAndDeadlinesStayInTheDenominator)
{
    SwitchFrameTickets tickets;
    int request = 0;
    tickets.accept(&request, 3, 0, 100);
    tickets.take(&request, 1);
    tickets.shown(1, 1000000000);
    tickets.shown(2, 2000000000);
    EXPECT_EQ(tickets.totals.shown, 2u);
    EXPECT_EQ(tickets.totals.unknownOrigin, 2u);
    EXPECT_EQ(tickets.totals.unknownDeadline, 2u);
    EXPECT_EQ(tickets.totals.missedDeadline, 0u);
}

TEST(SwitchFrameTickets, TimestampJitterDoesNotClaimAnEntireMissedRefresh)
{
    SwitchFrameTickets tickets;
    int first = 0, second = 0;
    tickets.accept(&first, 1, 20000, 10000);
    tickets.take(&first, 1);
    tickets.shown(1, 20100, 8333);
    EXPECT_EQ(tickets.totals.missedDeadline, 0u);
    tickets.accept(&second, 2, 36667, 30000);
    tickets.take(&second, 2);
    tickets.shown(2, 53334, 8333);
    EXPECT_EQ(tickets.totals.missedDeadline, 1u);
    EXPECT_EQ(tickets.totals.shown, 2u);
}

TEST(SwitchFrameTickets, CapacityNeverEvictsAnInFlightTicket)
{
    SwitchFrameTickets tickets;
    int requests[SwitchFrameTickets::kCapacity + 1] = {};
    for (unsigned i = 0; i < SwitchFrameTickets::kCapacity; ++i) {
        ASSERT_NE(tickets.accept(&requests[i], i, i + 1, i), 0u);
        tickets.take(&requests[i], i + 1);
    }
    EXPECT_EQ(tickets.accept(&requests[SwitchFrameTickets::kCapacity], 99, 1, 1), 0u);
    EXPECT_EQ(tickets.totals.overflow, 1u);
    EXPECT_EQ(tickets.byTake(1)->swapId, 0u);
    tickets.retire(&requests[7]);
    EXPECT_NE(tickets.accept(&requests[7], 123, 2, 2), 0u);
    EXPECT_EQ(tickets.find(&requests[7])->take, 0u);
    EXPECT_EQ(tickets.find(&requests[7])->gpu, 0u);
    EXPECT_EQ(tickets.totals.accepted, SwitchFrameTickets::kCapacity + 2);
}
#else
static_assert(std::is_empty<SwitchFrameTickets>::value, "Release carries no frame tracking state");

TEST(SwitchFrameTickets, ReleaseCompilesWithoutFrameTracking)
{
    SwitchFrameTickets tickets;
    int request = 0;
    EXPECT_EQ(tickets.accept(&request, 1, 2, 3), 0u);
    tickets.take(&request, 4);
    tickets.gpu(4, 5);
    EXPECT_EQ(tickets.find(&request), nullptr);
    EXPECT_EQ(tickets.byTake(4), nullptr);
    bool first = true;
    EXPECT_EQ(tickets.shown(4, 6, 0, &first).id, 0u);
    EXPECT_FALSE(first);
    tickets.retire(&request);
}
#endif
