// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "../SwitchDisplayRetirement.h"

namespace {
struct OriginalLegacySwapRequest {
    uint64_t sequence;
};

struct Display {
    static constexpr int kNoBuffer = -1;

    struct Ring {
        int pending = -1, superseded = -1, held = -1;
    } ring;

    struct PlaneSet {
        uint64_t seq = 0, pendingSeq = 0;
    } planeHeld, planeFencing;

    struct Latch {
        int armed = -1, superseded = -1;

        bool requested() const
        {
            return armed >= 0;
        }
    } planeLatch;

    std::array<PlaneSet, 4> planes = {};
    std::array<uint64_t, 4> entryPendingSeq = {}, entrySeq = {};
    switch_display::Retirement scanoutRetirement;
    bool latchRetire = true, planeHeldSet = false, planeFencePending = false, gpuScanout = false;
    const OriginalLegacySwapRequest *gpuRequest = nullptr;
    std::vector<OriginalLegacySwapRequest> queue;
    std::vector<switch_display::RetiredFrame> shown;
    std::vector<uint64_t> retired;
    unsigned kicks = 0;

    const PlaneSet &planeSetOf(int id) const
    {
        return planes.at(id);
    }

    const OriginalLegacySwapRequest *pendingSwapRequest() const
    {
        return queue.empty() ? nullptr : &queue.front();
    }

    uint64_t seqOf(const OriginalLegacySwapRequest *request) const
    {
        return request->sequence;
    }

    bool retireSwap(const OriginalLegacySwapRequest *request, bool)
    {
        if (request != pendingSwapRequest())
            return false;
        retired.push_back(request->sequence);
        queue.erase(queue.begin());
        return true;
    }

    void frameShown(uint64_t sequence, uint64_t at)
    {
        shown.push_back({sequence, at});
    }

    void kickSwapQueue()
    {
        ++kicks;
    }

    bool planeRecovering = false, planeRecoveryStuck = false;

    bool planeRecoveryWaits() const
    {
        return planeRecovering && !planeRecoveryStuck;
    }

    PlaneSet planeRecoveryTarget = {};

    struct Probe {
        bool on = false;

        bool active() const
        {
            return on;
        }
    } probe, planeProbe;

    bool kickDeferred = false, tapeSettle = false, probeRequest = false, hwSuspended = false, scanoutStopped = false;
    bool headHeldForTime = false;

    struct {
        bool wanted = false;
        PlaneSet held;
    } landscapeState;

#include "../SwitchRetireDisplay.h"
};

TEST(DisplayRetirement, AStuckRecoverysOverlaySetsNoLongerHoldRetirement)
{
    Display display;
    display.queue = {{1}, {2}};
    display.planeLatch.armed = 0;
    display.planes[0] = {1, 1};
    display.retireThrough(2, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeRecovering = display.planeRecoveryStuck = true;
    display.retireThrough(2, 110);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1, 2}));
}

TEST(DisplayRetirement, DeferredSwapGoesAgainAtAnIdleFrameEndAndOnlyThere)
{
    Display display;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 0u);
    display.kickDeferred = true;
    display.ring.pending = 0;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 0u);
    display.ring.pending = -1;
    display.planeLatch.armed = 1;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 0u);
    display.planeLatch.armed = -1;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 1u);
    EXPECT_FALSE(display.kickDeferred);
}

// A head held only for its requested presentation waits for no flip or overlay latch: its stamp names this frame end.
TEST(DisplayRetirement, ASwapHeldForItsTimeGoesAtEveryFrameEnd)
{
    Display display;
    display.kickDeferred = display.headHeldForTime = true;
    display.ring.pending = 0;
    display.planeLatch.armed = 1;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 1u);
    EXPECT_FALSE(display.kickDeferred);
    display.kickDeferred = true;
    display.probe.on = true;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 1u);
}

TEST(DisplayRetirement, FirstUiLatchWaitsForPairedOverlayThenCompletesWithoutAnotherUiFrame)
{
    Display display;
    display.queue = {{1}};
    display.planeLatch.armed = 0;
    display.planes[0] = {1, 1};
    display.retireThrough(1, 100);
    EXPECT_TRUE(display.shown.empty());
    EXPECT_TRUE(display.retired.empty());
    display.planeLatch.armed = -1;
    display.retireThrough(1, 110);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1}));
    ASSERT_EQ(display.shown.size(), 1u);
    EXPECT_EQ(display.shown[0].shownAt, 110u);
}

TEST(DisplayRetirement, EarlierUiDependencySurvivesNewerVideoOnlyLatch)
{
    Display display;
    display.queue = {{2}, {3}};
    display.ring.pending = 0;
    display.entrySeq[0] = display.entryPendingSeq[0] = 2;
    display.retireThrough(3, 100);
    EXPECT_TRUE(display.retired.empty());
    display.ring.pending = -1;
    display.retireThrough(2, 120);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{2, 3}));
    ASSERT_EQ(display.shown.size(), 1u);
    EXPECT_EQ(display.shown[0].sequence, 3u);
    EXPECT_EQ(display.shown[0].shownAt, 120u);
}

TEST(DisplayRetirement, NoChangeSwapExtendingCompletionDoesNotMoveOldestOverlayDependency)
{
    Display display;
    display.queue = {{1}, {2}, {3}};
    display.planeLatch.armed = 0;
    display.planes[0] = {3, 1};
    display.retireThrough(2, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeLatch.armed = -1;
    display.retireThrough(3, 110);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1, 2, 3}));
}

TEST(DisplayRetirement, NoChangeSwapExtendingUiRetainsOriginalPendingSequence)
{
    Display display;
    display.queue = {{1}, {2}, {3}};
    display.ring.pending = 0;
    display.entrySeq[0] = 3;
    display.entryPendingSeq[0] = 1;
    display.retireThrough(2, 100);
    EXPECT_TRUE(display.retired.empty());
    display.ring.pending = -1;
    display.retireThrough(3, 110);
    EXPECT_EQ(display.retired.size(), 3u);
}

TEST(DisplayRetirement, NewerPendingFrameDoesNotDelayEarlierCompletedFrame)
{
    Display display;
    display.queue = {{1}, {2}};
    display.planeLatch.armed = 0;
    display.planes[0] = {2, 2};
    display.retireThrough(1, 100);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1}));
    ASSERT_EQ(display.queue.size(), 1u);
    EXPECT_EQ(display.queue[0].sequence, 2u);
}

TEST(DisplayRetirement, SupersededAndHeldWindowsRetainTheirResources)
{
    Display display;
    display.queue = {{1}, {2}, {3}};
    display.planeLatch.armed = 1;
    display.planeLatch.superseded = 0;
    display.planes[0] = {1, 1};
    display.planes[1] = {2, 2};
    display.planeHeldSet = true;
    display.planeHeld = {3, 3};
    display.retireThrough(3, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeLatch.armed = display.planeLatch.superseded = -1;
    display.retireThrough(2, 110);
    EXPECT_TRUE(display.retired.empty());
    display.planeHeldSet = false;
    display.retireThrough(3, 120);
    EXPECT_EQ(display.retired.size(), 3u);
}

TEST(DisplayRetirement, PendingProducerFenceCannotBeRetiredByAnotherWindow)
{
    Display display;
    display.queue = {{1}, {2}};
    display.planeFencePending = true;
    display.planeFencing = {1, 0};
    display.retireThrough(2, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeFencePending = false;
    display.retireThrough(1, 110);
    EXPECT_EQ(display.retired.size(), 2u);
}

TEST(DisplayRetirement, DuplicateAndOutOfOrderNotificationsDoNotPublishAnotherFrame)
{
    Display display;
    display.queue = {{1}, {2}};
    display.retireThrough(2, 100);
    display.retireThrough(1, 110);
    display.retireThrough(2, 120);
    EXPECT_EQ(display.shown.size(), 1u);
    EXPECT_EQ(display.kicks, 1u);
}

TEST(DisplayRetirement, RecoveryKeepsOriginalTicketUntilRestoredCompositionLatches)
{
    Display display;
    display.queue = {{1}, {2}, {3}};
    display.planeRecovering = true;
    display.planeRecoveryTarget = {3, 1};
    display.retireThrough(3, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeRecovering = false;
    display.planeLatch.armed = 0;
    display.planes[0] = {3, 1};
    display.retireThrough(3, 110);
    EXPECT_TRUE(display.retired.empty());
    display.planeLatch.armed = -1;
    display.retireThrough(3, 120);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1, 2, 3}));
}

// A head deferred at its accept, then by every frame end's kick: the first kick at a later frame end reports it held,
// once; its take reports how long it waited.
TEST(HeldHead, AHeadAKickAtALaterFrameEndDefersIsReportedOnceAndAgainWhenTaken)
{
    switch_display::HeldHead held;
    switch_display::HeldHead::Ended ended;
    EXPECT_FALSE(held.defer(28867, 10, 1000, ended));
    EXPECT_FALSE(held.defer(28867, 10, 1100, ended));  // the kick's loop in the same frame
    EXPECT_TRUE(held.defer(28867, 11, 2000, ended));
    EXPECT_FALSE(held.defer(28867, 12, 3000, ended));
    EXPECT_EQ(ended.swap, 0u);
    const auto taken = held.take(28867, 5000);
    EXPECT_EQ(taken.swap, 28867u);
    EXPECT_TRUE(taken.taken);
    EXPECT_EQ(taken.ticks, 4000u);
    EXPECT_EQ(held.heldSinceBoot(), 1u);
    EXPECT_EQ(held.take(28867, 6000).swap, 0u);
}

TEST(HeldHead, AHeadTakenAtTheNextFrameEndIsNotHeld)
{
    switch_display::HeldHead held;
    switch_display::HeldHead::Ended ended;
    EXPECT_FALSE(held.recording());
    EXPECT_FALSE(held.defer(5, 20, 100, ended));
    EXPECT_TRUE(held.recording());
    EXPECT_EQ(held.take(5, 200).swap, 0u);
    EXPECT_FALSE(held.recording());
    EXPECT_FALSE(held.defer(6, 21, 300, ended));  // the next head counts from its own first deferral
    EXPECT_FALSE(held.defer(6, 21, 310, ended));
    EXPECT_EQ(held.heldSinceBoot(), 0u);
}

// The family cancelled a held head (IOMFB's swap_wait timeout): its hold ends untaken at the next kick of another head.
TEST(HeldHead, AHeldHeadTheFamilyLetGoEndsUntakenAtTheNextHead)
{
    switch_display::HeldHead held;
    switch_display::HeldHead::Ended ended;
    held.defer(7, 1, 10, ended);
    ASSERT_TRUE(held.defer(7, 2, 20, ended));
    EXPECT_FALSE(held.defer(8, 400, 9000, ended));
    EXPECT_EQ(ended.swap, 7u);
    EXPECT_FALSE(ended.taken);
    EXPECT_EQ(ended.ticks, 8990u);
    EXPECT_TRUE(held.defer(8, 401, 9100, ended));
    EXPECT_EQ(ended.swap, 0u);
    const auto other = held.take(9, 9200);
    EXPECT_EQ(other.swap, 8u);
    EXPECT_FALSE(other.taken);
    EXPECT_EQ(held.heldSinceBoot(), 2u);
}
}  // namespace
