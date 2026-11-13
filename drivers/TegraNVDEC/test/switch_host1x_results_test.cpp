// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <future>
#include <vector>
#include "drivers/TegraHost1x/SwitchHost1xResults.h"

template <uint32_t Engine> SwitchHost1xResultState switch_test_result_state;
template <uint32_t Engine> using TestResults = SwitchHost1xResults<Engine, switch_test_result_state<Engine>>;

template <uint32_t Engine>
static SwitchHost1xResult *reserve(uint64_t first, uint64_t last, switch_gpu_backing_token &token)
{
    using Results = TestResults<Engine>;
    EXPECT_TRUE(Results::start(162));
    auto *record = Results::allocate();
    EXPECT_NE(record, nullptr);
    EXPECT_EQ(Results::prepare(record, first, last, &token), 0);
    return record;
}

TEST(Host1xResults, FailedJobDoesNotPoisonUnrelatedLaterJob)
{
    using R = TestResults<101>;
    switch_gpu_backing_token a{}, b{};
    auto *first = reserve<101>(1, 1, a), *second = reserve<101>(2, 2, b);
    R::complete(&first, -5);
    R::complete(&second, 0);
    EXPECT_EQ(R::status(&a), -5);
    EXPECT_EQ(R::status(&b), 1);
    EXPECT_EQ(R::waitLegacy(1), -5);
    EXPECT_EQ(R::waitLegacy(2), 0);
}

TEST(Host1xResults, RetainedFailedTokenSurvivesLegacyHistoryEviction)
{
    using R = TestResults<102>;
    switch_gpu_backing_token held{}, next{};
    auto *record = reserve<102>(1, 1, held);
    ASSERT_EQ(R::retain(&held), 0);
    R::complete(&record, -125);
    for (uint64_t i = 2; i <= 100; ++i) {
        record = reserve<102>(i, i, next);
        R::complete(&record, 0);
    }
    EXPECT_EQ(R::status(&held), -125);
    R::release(&held);
    EXPECT_EQ(R::status(&held), -125);
    EXPECT_EQ(R::wait(&held), -125);
    EXPECT_EQ(R::waitLegacy(1), -125);
}

TEST(Host1xResults, JobAgedOutOfHistoryReadsCompleted)
{
    using R = TestResults<109>;
    switch_gpu_backing_token old{}, next{};
    auto *record = reserve<109>(1, 1, old);
    R::physical(record, 1);
    R::complete(&record, 0);
    for (uint64_t i = 2; i <= 100; ++i) {
        record = reserve<109>(i, i, next);
        R::complete(&record, 0);
    }
    EXPECT_EQ(R::status(&old), 1);
    EXPECT_EQ(R::wait(&old), 1);
    EXPECT_EQ(R::physicalStatus(&old), 1);
    EXPECT_EQ(R::waitLegacy(1), 0);
    EXPECT_EQ(R::retain(&old), 0);
    R::release(&old);
    EXPECT_EQ(R::status(&old), 1);
}

TEST(Host1xResults, AgedOutBatchKeepsItsFailureForEveryFence)
{
    using R = TestResults<110>;
    switch_gpu_backing_token batch{}, next{};
    auto *record = reserve<110>(3, 5, batch);
    R::physical(record, -5);
    R::complete(&record, -5);
    for (uint64_t i = 6; i <= 100; ++i) {
        record = reserve<110>(i, i, next);
        R::complete(&record, 0);
    }
    EXPECT_EQ(R::status(&batch), -5);
    EXPECT_EQ(R::physicalStatus(&batch), -5);
    EXPECT_EQ(R::waitLegacy(3), -5);
    EXPECT_EQ(R::waitLegacy(4), -5);
    EXPECT_EQ(R::waitLegacy(6), 0);
}

TEST(Host1xResults, UnissuedOrForeignWorkStaysStale)
{
    using R = TestResults<111>;
    switch_gpu_backing_token token{};
    auto *record = reserve<111>(1, 1, token);
    R::complete(&record, 0);
    switch_gpu_backing_token future = token, otherGeneration = token, otherQueue = token;
    future.value = 2;
    otherGeneration.reset_generation += 1;
    otherQueue.queue += 1;
    EXPECT_EQ(R::status(&future), SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(R::waitLegacy(2), SWITCH_GPU_BACKING_STALE);
    for (uint64_t i = 2; i <= 100; ++i) {
        record = reserve<111>(i, i, future);
        R::complete(&record, 0);
    }
    future.value = 101;
    EXPECT_EQ(R::status(&future), SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(R::status(&otherGeneration), SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(R::status(&otherQueue), SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(R::retain(&otherQueue), SWITCH_GPU_BACKING_STALE);
}

TEST(Host1xResults, CancelledEscapedTokenCannotAliasReusedHardwareThreshold)
{
    using R = TestResults<103>;
    switch_gpu_backing_token cancelled{}, accepted{};
    auto *record = reserve<103>(1, 1, cancelled);
    ASSERT_EQ(R::retain(&cancelled), 0);
    R::cancel(&record);
    record = reserve<103>(1, 1, accepted);
    EXPECT_NE(cancelled.reset_generation, accepted.reset_generation);
    R::complete(&record, 0);
    EXPECT_EQ(R::status(&cancelled), -125);
    EXPECT_EQ(R::waitLegacy(1), 0);
    EXPECT_EQ(R::status(&accepted), 1);
    R::release(&cancelled);
    EXPECT_EQ(R::status(&accepted), 1);
}

TEST(Host1xResults, PrivateAdmissionRetryPreservesQueueGeneration)
{
    using R = TestResults<104>;
    switch_gpu_backing_token a{}, b{};
    auto *record = reserve<104>(1, 1, a);
    R::cancel(&record);
    record = reserve<104>(1, 1, b);
    EXPECT_EQ(a.reset_generation, b.reset_generation);
    R::cancel(&record);
}

TEST(Host1xResults, LegacyIntermediateFenceSharesBatchOutcome)
{
    using R = TestResults<105>;
    switch_gpu_backing_token token{};
    auto *record = reserve<105>(7, 9, token);
    R::complete(&record, -5);
    EXPECT_EQ(R::waitLegacy(7), -5);
    EXPECT_EQ(R::waitLegacy(8), -5);
    EXPECT_EQ(R::waitLegacy(9), -5);
}

TEST(Host1xResults, CompletionWakesSleepingRetainedWaiter)
{
    using R = TestResults<106>;
    switch_gpu_backing_token token{};
    auto *record = reserve<106>(1, 1, token);
    auto future = std::async(std::launch::async, [&] {
        return R::wait(&token);
    });
    R::complete(&record, -5);
    EXPECT_EQ(future.get(), -5);
}

TEST(Host1xResults, FullSoftwareSequencePreservesLow32Wrap)
{
    using R = TestResults<107>;
    switch_gpu_backing_token token{};
    auto *record = reserve<107>(uint64_t(1) << 32, uint64_t(1) << 32, token);
    EXPECT_EQ(uint32_t(token.value), 0u);
    R::complete(&record, 0);
    EXPECT_EQ(R::status(&token), 1);
}

TEST(Host1xResults, BoundedPoolReusesReturnedRecordsWithoutLosingLiveOnes)
{
    using R = TestResults<108>;
    ASSERT_TRUE(R::start(162));
    std::vector<SwitchHost1xResult *> records;
    for (uint32_t i = 0; i < 4096; ++i) {
        auto *p = R::allocate();
        ASSERT_NE(p, nullptr);
        records.push_back(p);
    }
    EXPECT_EQ(R::allocate(), nullptr);
    R::cancel(&records[0]);
    records[0] = R::allocate();
    ASSERT_NE(records[0], nullptr);
    for (auto *&record : records)
        R::cancel(&record);
}
