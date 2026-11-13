// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <deque>
#include "../SwitchVICQueue.h"

TEST(SwitchVICQueue, ConfigurationsAreNotReusedUntilTheirFenceRetires)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.reset(40));
    for (uint32_t i = 0; i < queue.kSlots; ++i) {
        EXPECT_EQ(queue.tail(), i);
        ASSERT_TRUE(queue.publish({0, i, nullptr, nullptr}));
    }
    EXPECT_FALSE(queue.publish({}));
    EXPECT_FALSE(queue.retire(40));
    EXPECT_EQ(queue.count, queue.kSlots);
    EXPECT_TRUE(queue.retire(41));
    EXPECT_EQ(queue.tail(), 0u);
    EXPECT_TRUE(queue.publish({}));
    EXPECT_EQ(queue.jobs[0].fence, 57u);
    EXPECT_EQ(queue.jobs[queue.head].fence, 42u);
}

TEST(SwitchVICQueue, CompletionAndAdmissionSurviveSyncpointWrap)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.reset(0xfffffffe));
    ASSERT_TRUE(queue.publish({}));
    ASSERT_TRUE(queue.publish({}));
    EXPECT_EQ(queue.submitted, 0x100000000ULL);
    EXPECT_FALSE(queue.retire(0xfffffffe));
    EXPECT_TRUE(queue.retire(0xffffffff));
    EXPECT_TRUE(queue.retire(0));
    EXPECT_EQ(queue.completed, 0x100000000ULL);
    EXPECT_EQ(queue.count, 0u);
}

TEST(SwitchVICQueue, ResetCannotForgetLiveConfigurations)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.publish({}));
    EXPECT_FALSE(queue.reset(123));
    EXPECT_EQ(queue.submitted, 1u);
    EXPECT_TRUE(queue.retire(1));
    EXPECT_TRUE(queue.reset(123));
    EXPECT_EQ(queue.completed, 123u);
}

TEST(SwitchVICQueue, PublishedCallbacksStayWithTheirOwnFence)
{
    SwitchVICQueue queue;
    int first = 0, second = 0;
    auto completion = [](void *p, int value) {
        *static_cast<int *>(p) = value;
    };
    ASSERT_TRUE(queue.publish({0, 10, completion, &first}));
    ASSERT_TRUE(queue.publish({0, 30, completion, &second}));
    const auto job = queue.jobs[queue.head];
    ASSERT_TRUE(queue.retire(1));
    job.completion(job.context, 7);
    EXPECT_EQ(first, 7);
    EXPECT_EQ(second, 0);
    EXPECT_EQ(queue.jobs[queue.head].context, &second);
    EXPECT_EQ(queue.jobs[queue.head].fence, 2u);
}

TEST(SwitchVICQueue, WholeBatchesWaitUntilAllConfigurationSlotsAreFree)
{
    SwitchVICQueue queue;
    for (uint32_t i = 0; i < 15; ++i)
        ASSERT_TRUE(queue.publish({}));
    EXPECT_TRUE(queue.canPublish(1));
    EXPECT_FALSE(queue.canPublish(0));
    EXPECT_FALSE(queue.canPublish(2));
    EXPECT_FALSE(queue.canPublish(17));
    ASSERT_TRUE(queue.retire(1));
    ASSERT_TRUE(queue.retire(2));
    ASSERT_TRUE(queue.canPublish(3));
    EXPECT_EQ(queue.configurationSlot(0), 15u);
    EXPECT_EQ(queue.configurationSlot(1), 0u);
    EXPECT_EQ(queue.configurationSlot(2), 1u);
    EXPECT_EQ(queue.jobs[queue.head].fence, 3u);
}

TEST(SwitchVICQueue, MixedBatchAdmissionAndRetirementPreserveEveryLiveConfiguration)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.reset(0xffffff00));
    std::deque<uint64_t> expected;
    uint32_t random = 0x213807u;
    uint64_t sequence = 0;
    for (uint32_t iteration = 0; iteration < 100000; ++iteration) {
        random = random * 1664525u + 1013904223u;
        const uint32_t batch = 1 + (random >> 16) % 3;
        if ((random & 1) && queue.canPublish(batch)) {
            uint32_t slots[3];
            for (uint32_t i = 0; i < batch; ++i)
                slots[i] = queue.configurationSlot(i);
            for (uint32_t i = 0; i < batch; ++i) {
                ASSERT_EQ(queue.tail(), slots[i]);
                ASSERT_TRUE(queue.publish({0, ++sequence, nullptr, nullptr}));
                expected.push_back(sequence);
            }
        } else if (queue.count) {
            ASSERT_EQ(queue.jobs[queue.head].submittedUs, expected.front());
            ASSERT_TRUE(queue.retire(static_cast<uint32_t>(queue.jobs[queue.head].fence)));
            expected.pop_front();
        }
        ASSERT_EQ(queue.count, expected.size());
        for (uint32_t i = 0; i < queue.count; ++i)
            ASSERT_EQ(queue.jobs[(queue.head + i) % queue.kSlots].submittedUs, expected[i]);
    }
}

TEST(SwitchVICQueue, ResumePreservesCompletedTokenIdentityAcrossWrap)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.reset(0xfffffffe));
    ASSERT_TRUE(queue.publish({}));
    EXPECT_FALSE(queue.rebase(0));
    ASSERT_TRUE(queue.retire(0xffffffff));
    const uint64_t oldToken = queue.completed;
    ASSERT_TRUE(queue.rebase(1));
    EXPECT_EQ(queue.completed, 0x100000001ULL);
    EXPECT_LT(oldToken, queue.completed);
    ASSERT_TRUE(queue.publish({}));
    EXPECT_EQ(queue.jobs[queue.head].fence, 0x100000002ULL);
    EXPECT_FALSE(queue.retire(1));
    EXPECT_TRUE(queue.retire(2));
    EXPECT_FALSE(queue.rebase(1));
}

TEST(SwitchVICQueue, LegacyWaitResolvesOnlyIssuedThresholdsInTheCurrentWindow)
{
    SwitchVICQueue queue;
    uint64_t sequence = 0;
    EXPECT_FALSE(queue.legacySequence(1, &sequence));
    ASSERT_TRUE(queue.reset(0xfffffffe));
    ASSERT_TRUE(queue.publish({}));
    ASSERT_TRUE(queue.publish({}));
    ASSERT_TRUE(queue.legacySequence(0xffffffff, &sequence));
    EXPECT_EQ(sequence, 0xffffffffULL);
    ASSERT_TRUE(queue.legacySequence(0, &sequence));
    EXPECT_EQ(sequence, 0x100000000ULL);
    EXPECT_FALSE(queue.legacySequence(1, &sequence));
    EXPECT_FALSE(queue.legacySequence(0x7fffffff, &sequence));
}

TEST(SwitchVICQueue, ExhaustedSoftwareSequenceRejectsWithoutWrapping)
{
    SwitchVICQueue queue;
    queue.submitted = queue.completed = UINT64_MAX - 1;
    EXPECT_FALSE(queue.canPublish(2));
    EXPECT_TRUE(queue.publish({}));
    EXPECT_FALSE(queue.publish({}));
    EXPECT_EQ(queue.submitted, UINT64_MAX);
    EXPECT_TRUE(queue.retire(UINT32_MAX));
    EXPECT_FALSE(queue.rebase(0));
}

TEST(SwitchVICQueue, EveryJobOfABatchFindsTheBridgeItsStreamWaitsOn)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.reset(30));
    ASSERT_TRUE(queue.publish({}));
    auto *bridge = reinterpret_cast<switch_gpu_bridge *>(uintptr_t(0x40));
    for (uint32_t j = 0; j < 3; ++j)
        ASSERT_TRUE(queue.publish({0, 1, nullptr, nullptr, j == 2 ? bridge : nullptr, nullptr, nullptr, 2 - j}));
    EXPECT_EQ(queue.jobs[queue.batchEnd()].bridge, nullptr);
    EXPECT_EQ(queue.jobs[queue.last()].bridge, bridge);
    ASSERT_TRUE(queue.retire(31));
    for (uint32_t fence = 32; fence <= 34; ++fence) {
        EXPECT_EQ(queue.jobs[queue.batchEnd()].bridge, bridge);
        ASSERT_TRUE(queue.retire(fence));
    }
}
