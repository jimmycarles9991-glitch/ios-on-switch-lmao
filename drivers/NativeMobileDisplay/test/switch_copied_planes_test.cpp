// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchDisplayComposition.h"

namespace {
constexpr int kIOReturnAborted = -14, kIOReturnSuccess = 0;

struct Display {
    using PlaneSet = switch_display::Composition;

    struct Job {
        uint64_t request = 7;
        int status = 0;
    } job;

    struct Latch {
        int active = 0;
    } planeLatch;

    PlaneSet planeNext, planeTarget, planeHeld, active, fenced;
    bool planeNextSet = true, planeHeldSet = false, needsFence = true, accepted = true;
    unsigned completions = 0, fenceSubmissions = 0;
    int completedStatus = 0;
    bool joinedWhenCompleted = true;

    uint64_t seqOf(uint64_t request) const
    {
        return request;
    }

    bool planeFenced(const PlaneSet &) const
    {
        return needsFence;
    }

    bool planeFence(const PlaneSet &set)
    {
        ++fenceSubmissions;
        if (!accepted)
            return false;
        fenced = planeTarget = set;
        return true;
    }

    const PlaneSet &planeSetOf(int) const
    {
        return active;
    }

    void completeCopiedJob()
    {
        ++completions;
        completedStatus = job.status;
        joinedWhenCompleted = copyPlaneJoined;
    }

#include "../SwitchCopiedPlanes.h"

    Display()
    {
        planeNext.window[0].on = planeNext.window[1].on = true;
        planeNext.window[0].slot = 1;
        planeNext.window[1].slot = 2;
        active.window[0].on = true;
        active.window[0].slot = 3;
    }
};

TEST(CopiedPlanes, PixelsFirstRemainOwnedUntilBothOverlayProducersFinish)
{
    Display display;
    ASSERT_TRUE(display.beginCopiedPlaneFence());
    EXPECT_EQ(display.fenced.seq, 7u);
    EXPECT_TRUE(display.fenced.uses(1));
    EXPECT_TRUE(display.fenced.uses(2));
    EXPECT_TRUE(display.copiedPixelsFinished());
    EXPECT_EQ(display.completions, 0u);
    EXPECT_TRUE(display.copyPlaneJoined);
    display.copiedPlaneFenceFinished(0);
    EXPECT_EQ(display.completions, 1u);
    EXPECT_EQ(display.completedStatus, 0);
    EXPECT_FALSE(display.joinedWhenCompleted);
    EXPECT_TRUE(display.planeNextSet);
}

TEST(CopiedPlanes, HardwareFenceFirstDoesNotPublishUnfinishedCpuPixels)
{
    Display display;
    ASSERT_TRUE(display.beginCopiedPlaneFence());
    display.copiedPlaneFenceFinished(0);
    EXPECT_EQ(display.completions, 0u);
    EXPECT_TRUE(display.copyPlaneJoined);
    EXPECT_TRUE(display.copiedPixelsFinished());
    EXPECT_EQ(display.completions, 1u);
    EXPECT_FALSE(display.copyPlaneJoined);
}

TEST(CopiedPlanes, LostOverlayWaitsForCpuWriterToStopBeforeDiscardingFrame)
{
    Display display;
    ASSERT_TRUE(display.beginCopiedPlaneFence());
    display.copiedPlaneFenceFinished(-5);
    EXPECT_EQ(display.completions, 0u);
    EXPECT_TRUE(display.planeNextSet);
    EXPECT_TRUE(display.copiedPixelsFinished());
    EXPECT_EQ(display.completions, 1u);
    EXPECT_EQ(display.completedStatus, kIOReturnAborted);
    EXPECT_FALSE(display.planeNextSet);
    EXPECT_TRUE(display.planeTarget.uses(3));
    EXPECT_FALSE(display.planeTarget.uses(1));
}

TEST(CopiedPlanes, CpuFailureKeepsItsStatusUntilHardwareRetirement)
{
    Display display;
    ASSERT_TRUE(display.beginCopiedPlaneFence());
    display.job.status = -22;
    EXPECT_TRUE(display.copiedPixelsFinished());
    EXPECT_EQ(display.completions, 0u);
    display.copiedPlaneFenceFinished(0);
    EXPECT_EQ(display.completions, 1u);
    EXPECT_EQ(display.completedStatus, -22);
    EXPECT_FALSE(display.planeNextSet);
    EXPECT_TRUE(display.planeTarget.uses(3));
}

TEST(CopiedPlanes, FailedProducerRestoresPendingVisibleComposition)
{
    Display display;
    display.planeHeldSet = true;
    display.planeHeld.window[1].on = true;
    display.planeHeld.window[1].slot = 4;
    ASSERT_TRUE(display.beginCopiedPlaneFence());
    EXPECT_TRUE(display.copiedPixelsFinished());
    display.copiedPlaneFenceFinished(-5);
    EXPECT_TRUE(display.planeTarget.uses(4));
    EXPECT_FALSE(display.planeTarget.uses(3));
}

TEST(CopiedPlanes, AlreadyVisibleOrAbsentOverlayDoesNotSubmitHardwareWork)
{
    Display display;
    display.needsFence = false;
    EXPECT_TRUE(display.beginCopiedPlaneFence());
    EXPECT_FALSE(display.copiedPixelsFinished());
    EXPECT_EQ(display.fenceSubmissions, 0u);
    display.needsFence = true;
    display.planeNextSet = false;
    EXPECT_TRUE(display.beginCopiedPlaneFence());
    EXPECT_FALSE(display.copiedPixelsFinished());
    EXPECT_EQ(display.fenceSubmissions, 0u);
}

TEST(CopiedPlanes, AdmissionFailureLeavesNoCompletionJoin)
{
    Display display;
    display.accepted = false;
    EXPECT_FALSE(display.beginCopiedPlaneFence());
    EXPECT_FALSE(display.copyPlaneJoined);
    EXPECT_FALSE(display.copiedPixelsFinished());
    EXPECT_EQ(display.completions, 0u);
}
}  // namespace
