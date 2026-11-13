// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include "../../TegraGM20B/SwitchGpuBacking.h"
#include "bazel/testing/source_text.h"

namespace {
unsigned fences, completions;
int fenceResult, completedStatus;
uint32_t fencedCount;
switch_gpu_backing_token fenced[2];

void native_legacy_gpu_complete(void *, int status)
{
    ++completions;
    completedStatus = status;
}

int switch_host1x_token_fence(const switch_gpu_backing_token *tokens, uint32_t count, void (*completed)(void *, int),
                              void *)
{
    EXPECT_EQ(completed, native_legacy_gpu_complete);
    ++fences;
    fencedCount = count;
    for (uint32_t i = 0; i < count; ++i)
        fenced[i] = tokens[i];
    return fenceResult;
}

struct Display {
    static constexpr unsigned kPlaneDependencies = 2;

    struct PlaneSet {
        switch_gpu_backing_token producers[2];
        unsigned count;
    };

    int planeFenceWithMediaFlip(const PlaneSet &, const switch_gpu_backing_token *producers, unsigned count)
    {
        return switch_host1x_token_fence(producers, count, native_legacy_gpu_complete, this);
    }

    unsigned planeDependencies(const PlaneSet &p, switch_gpu_backing_token tokens[2]) const
    {
        for (unsigned i = 0; i < p.count; ++i)
            tokens[i] = p.producers[i];
        return p.count;
    }

#include "../SwitchMediaPlaneFence.h"
};

constexpr switch_gpu_backing_token kDecode = {0x100000007ULL, 1, SWITCH_GPU_ENGINE_NVDEC, 162};
constexpr switch_gpu_backing_token kConvert = {0x200000009ULL, 1, SWITCH_GPU_ENGINE_VIC, 161};
constexpr switch_gpu_backing_token kRender = {12, 1, SWITCH_GPU_ENGINE_GM20B, 3};

class MediaPlaneFence : public testing::Test {
protected:
    Display display;

    void SetUp() override
    {
        fences = completions = fencedCount = 0;
        fenceResult = completedStatus = 0;
    }
};

TEST_F(MediaPlaneFence, AVideoFrameIsArmedOnItsProducersSyncpointsWithNoCompletionYet)
{
    EXPECT_TRUE(display.planeMediaFence({{kDecode, kConvert}, 2}));
    EXPECT_EQ(fences, 1u);
    EXPECT_EQ(fencedCount, 2u);
    EXPECT_EQ(fenced[0].value, kDecode.value);
    EXPECT_EQ(fenced[1].queue, kConvert.queue);
    EXPECT_EQ(completions, 0u);
}

TEST_F(MediaPlaneFence, AGpuProducedSurfaceOrNoProducerStaysOnTheBridge)
{
    EXPECT_FALSE(display.planeMediaFence({{kConvert, kRender}, 2}));
    EXPECT_FALSE(display.planeMediaFence({{}, 0}));
    EXPECT_EQ(fences, 0u);
}

TEST_F(MediaPlaneFence, NoFreeFlipSlotStaysOnTheBridge)
{
    fenceResult = -16;
    EXPECT_FALSE(display.planeMediaFence({{kConvert}, 1}));
    EXPECT_EQ(completions, 0u);
}

TEST_F(MediaPlaneFence, AProducerThatAlreadyFailedCompletesOnceWithItsError)
{
    fenceResult = -5;
    EXPECT_TRUE(display.planeMediaFence({{kConvert}, 1}));
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(completedStatus, -5);
}

// planeFence keeps its state (the pending set, the deadline) and tries the engines' interrupts before it queues the
// copy thread's bridge fence.
TEST(MediaPlaneFenceSource, ThePlaneFenceTriesTheMediaFenceBeforeTheCopyThread)
{
    using switch_ios::test::SourceText;
    const std::string fence = SourceText("../NativePlaneFenceDisplay.h");
    const auto at = [&](const char *text) {
        return fence.find(text);
    };
    ASSERT_NE(at("if (!planeMediaFence(p)) {"), std::string::npos);
    EXPECT_LT(at("gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);"), at("if (!planeMediaFence(p)) {"));
    EXPECT_LT(at("if (!planeMediaFence(p)) {"), at("planeFenceQueued = true;"));
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    EXPECT_THAT(display, testing::HasSubstr("#include \"SwitchMediaPlaneFence.h\""));
}
}  // namespace
