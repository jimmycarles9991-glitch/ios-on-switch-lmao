// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <functional>
#include "../../TegraGM20B/SwitchGpuBacking.h"

namespace {
struct Display;
constexpr int kIOReturnAborted = -14;
unsigned submissions, roomWaits, completions, legacyCalls, submittedCount;
int busy, submitError, roomError, completedStatus;
switch_gpu_backing_token submitted;
switch_gpu_backing_token submittedSecond;
std::function<void()> onRoom, onComplete;

void native_legacy_gpu_complete(void *, int status)
{
    ++completions;
    completedStatus = status;
    if (onComplete)
        onComplete();
}

int switch_gpu_bridge_fence(const switch_gpu_backing_token *tokens, uint32_t count, void (*completed)(void *, int),
                            void *context)
{
    EXPECT_LE(count, 2u);
    submittedCount = count;
    if (count == 2)
        submittedSecond = tokens[1];
    ++submissions;
    submitted = tokens[0];
    if (busy-- > 0)
        return -16;
    if (submitError)
        return submitError;
    if (onComplete)
        completed(context, 0);
    return 0;
}

int switch_gpu_bridge_wait_room()
{
    ++roomWaits;
    if (onRoom)
        onRoom();
    return roomError;
}

int tegra_gm20b_scanout_fence(void *, uint64_t)
{
    ++legacyCalls;
    return submitError;
}

struct Display {
    struct Snapshot {
        void *lease;

        struct {
            switch_gpu_backing_token producer;
        } view;
    } snapshot[2] = {};

    struct Slot {
        Snapshot *native;
    } planeSlots[2] = {{&snapshot[0]}, {&snapshot[1]}};

    struct Set {
        unsigned count = 1;
    } planeFencing;

    static constexpr unsigned kPlaneDependencies = 6;

    unsigned planeDependencies(const Set &p, switch_gpu_backing_token tokens[kPlaneDependencies]) const
    {
        for (unsigned i = 0; i < p.count; ++i)
            tokens[i] = planeSlots[i].native->view.producer;
        return p.count;
    }

    struct Timer {
        unsigned cancels = 0;

        void cancelTimeout()
        {
            ++cancels;
        }
    } timer;

    Timer *gpuDeadline = &timer;
    bool copyStopping = false, planeFencePending = true, gpuScanout = true;

    uint64_t planeFencePa(const Set &)
    {
        return 0x80000000;
    }

#include "../SwitchPlaneFenceAdmission.h"
};

class PlaneFenceAdmission : public testing::Test {
protected:
    Display display;

    void SetUp() override
    {
        submissions = roomWaits = completions = legacyCalls = 0;
        busy = submitError = roomError = completedStatus = 0;
        submitted = submittedSecond = {};
        submittedCount = 0;
        onRoom = onComplete = {};
        display.snapshot[0].lease = &display;
        display.snapshot[0].view.producer = {0x100000123ULL, 5, SWITCH_GPU_ENGINE_NVDEC, 162};
        display.planeFenceQueued = true;
    }
};

TEST_F(PlaneFenceAdmission, DispatchPreservesExactProducerAndDoesNotWaitForCompletion)
{
    display.planeFenceWork();
    EXPECT_EQ(submissions, 1u);
    EXPECT_EQ(roomWaits, 0u);
    EXPECT_EQ(completions, 0u);
    EXPECT_EQ(legacyCalls, 0u);
    EXPECT_EQ(submitted.value, 0x100000123ULL);
    EXPECT_EQ(submitted.reset_generation, 5u);
    EXPECT_TRUE(display.planeFencePending);
    EXPECT_FALSE(display.planeFenceQueued);
}

TEST_F(PlaneFenceAdmission, RingPressureWaitsOnlyForRoomAndKeepsOriginalProducer)
{
    busy = 2;
    onRoom = [&] {
        EXPECT_TRUE(display.planeFencePending);
        EXPECT_TRUE(display.gpuScanout);
    };
    display.planeFenceWork();
    EXPECT_EQ(roomWaits, 2u);
    EXPECT_EQ(submissions, 3u);
    EXPECT_EQ(completions, 0u);
}

TEST_F(PlaneFenceAdmission, FailureCompletesOnceWithoutResubmitting)
{
    submitError = -5;
    display.planeFenceWork();
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(completedStatus, -5);
    EXPECT_EQ(submissions, 1u);
}

TEST_F(PlaneFenceAdmission, CompletionMayRunBeforeAdmissionReturns)
{
    onComplete = [&] {
        display.planeSlots[0].native = nullptr;
        display.planeFencePending = false;
    };
    display.planeFenceWork();
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(completedStatus, 0);
}

TEST_F(PlaneFenceAdmission, StopBeforeAdmissionAndDuringRoomWaitCompletesAborted)
{
    display.copyStopping = true;
    display.planeFenceWork();
    EXPECT_EQ(submissions, 0u);
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(completedStatus, kIOReturnAborted);
    display.copyStopping = false;
    busy = 1;
    onRoom = [&] {
        display.copyStopping = true;
    };
    display.planeFenceWork();
    EXPECT_EQ(completions, 2u);
    EXPECT_EQ(completedStatus, kIOReturnAborted);
}

TEST_F(PlaneFenceAdmission, FailedRoomReservationIsReportedOnce)
{
    busy = 1;
    roomError = -19;
    display.planeFenceWork();
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(completedStatus, -19);
    EXPECT_EQ(submissions, 1u);
}

TEST_F(PlaneFenceAdmission, TeardownCancelsOnlyAnUndispatchedFence)
{
    display.cancelQueuedPlaneFence();
    EXPECT_FALSE(display.gpuScanout);
    EXPECT_FALSE(display.planeFencePending);
    EXPECT_EQ(display.timer.cancels, 1u);
    display.gpuScanout = display.planeFencePending = true;
    display.cancelQueuedPlaneFence();
    EXPECT_TRUE(display.gpuScanout);
    EXPECT_TRUE(display.planeFencePending);
    EXPECT_EQ(display.timer.cancels, 1u);
}

TEST_F(PlaneFenceAdmission, TwoIndependentProducersTravelInOneHardwareFence)
{
    display.snapshot[1].lease = &display;
    display.snapshot[1].view.producer = {0x200000abcULL, 8, SWITCH_GPU_ENGINE_VIC, 163};
    display.planeFencing.count = 2;
    busy = 1;
    onComplete = [&] {
        display.planeSlots[0].native = display.planeSlots[1].native = nullptr;
    };
    display.planeFenceWork();
    EXPECT_EQ(submittedCount, 2u);
    EXPECT_EQ(submitted.engine, SWITCH_GPU_ENGINE_NVDEC);
    EXPECT_EQ(submittedSecond.engine, SWITCH_GPU_ENGINE_VIC);
    EXPECT_EQ(submittedSecond.value, 0x200000abcULL);
    EXPECT_EQ(submittedSecond.reset_generation, 8u);
    EXPECT_EQ(roomWaits, 1u);
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(legacyCalls, 0u);
}
}  // namespace
