// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <thread>
#include "../raw/runtime/raw_event_timeline.h"
#include "../raw/runtime/raw_submit_queue.h"

namespace {
using raw::EventDependencyState;
using raw::EventOutcome;
using raw::EventTimeline;

TEST(EventTimeline, ConsumerRunsBeforeCpuCompletion)
{
    raw::SubmitQueue<uintptr_t, int> ready;
    EventTimeline events;
    const auto consumerReservation = ready.reserve(1, 10);
    const auto producerReservation = ready.reserve(2, 20);
    ASSERT_TRUE(ready.commit(1, 10, consumerReservation));
    ASSERT_TRUE(ready.commit(2, 20, producerReservation));
    auto consumer = ready.next();
    ASSERT_EQ(consumer.value, 10);
    ASSERT_FALSE(events.observe(7, 9, 1, 4));
    ready.park(consumer);
    const auto producer = ready.next();
    ASSERT_EQ(producer.value, 20);
    std::vector<uintptr_t> wake;
    const auto signal = events.publish(7, 9, {42, 4}, wake);
    for (const auto queue : wake)
        ready.wake(queue);
    ready.retire(producer);
    consumer = ready.next();
    ASSERT_EQ(consumer.value, 10);
    const auto dependency = events.observe(7, 9, 1, 4);
    ASSERT_EQ(dependency, signal);
    EXPECT_EQ(signal->outcome(), EventOutcome::pending);
    EXPECT_EQ(raw::eventDependencies({dependency}, 4, true), EventDependencyState::ready);
    EXPECT_EQ(raw::eventDependencies({dependency}, 4, false), EventDependencyState::blocked);
}

TEST(EventTimeline, FailureWakesConsumerWithoutSuccessfulSignal)
{
    EventTimeline events;
    std::vector<uintptr_t> wake;
    auto signal = events.publish(7, 9, {42, 4}, wake);
    auto dependency = events.observe(7, 9, 1, 4);
    ASSERT_EQ(dependency, signal);
    ASSERT_TRUE(events.retire(7, signal, false, &wake));
    EXPECT_EQ(wake, std::vector<uintptr_t>{1});
    EXPECT_EQ(raw::eventDependencies({dependency}, 4, true), EventDependencyState::failed);
    EXPECT_FALSE(signal->settle(true));
    EXPECT_EQ(events.size(), 0);
}

TEST(EventTimeline, ResetCannotReusePendingOrdering)
{
    EventTimeline events;
    std::vector<uintptr_t> wake;
    auto old = events.publish(7, 10, {42, 4}, wake);
    EXPECT_FALSE(events.observe(7, 10, 1, 5));
    EXPECT_EQ(raw::eventDependencies({old}, 5, true), EventDependencyState::blocked);
    auto replacement = events.publish(7, 10, {42, 5}, wake);
    EXPECT_EQ(events.observe(7, 10, 1, 5), replacement);
    events.retire(7, old, false);
    EXPECT_EQ(raw::eventDependencies({old}, 5, true), EventDependencyState::failed);
    EXPECT_EQ(raw::eventDependencies({replacement}, 5, true), EventDependencyState::ready);
}

TEST(EventTimeline, SuccessfulPrefixSurvivesLaterFailure)
{
    EventTimeline events;
    std::vector<uintptr_t> wake;
    auto prefix = events.publish(7, 8, {10, 2}, wake);
    auto suffix = events.publish(7, 9, {11, 2}, wake);
    events.retire(7, prefix, true);
    events.retire(7, suffix, false);
    EXPECT_EQ(prefix->outcome(), EventOutcome::succeeded);
    EXPECT_EQ(suffix->outcome(), EventOutcome::failed);
    EXPECT_EQ(raw::eventDependencies({prefix}, 3, false), EventDependencyState::ready);
    EXPECT_EQ(events.size(), 0);
}

TEST(EventTimeline, IndependentSignalsAtSameValueRetainExactOutcomes)
{
    EventTimeline events;
    std::vector<uintptr_t> wake;
    auto first = events.publish(1, 8, {10, 2}, wake);
    auto second = events.publish(1, 8, {11, 2}, wake);
    auto consumer = events.observe(1, 8, 1, 2);
    events.retire(1, first, false);
    EXPECT_EQ(raw::eventDependencies({consumer}, 2, true), EventDependencyState::failed);
    EXPECT_EQ(events.observe(1, 8, 1, 2), second);
    events.retire(1, second, true);
    EXPECT_EQ(events.size(), 0);
}

TEST(EventTimeline, OnlyMatchingEventAndValueWake)
{
    EventTimeline events;
    std::vector<uintptr_t> wake;
    for (uintptr_t key = 1; key <= 10000; ++key)
        ASSERT_FALSE(events.observe(key, key, key, 1));
    ASSERT_FALSE(events.observe(7000, 7001, 10001, 1));
    const auto signal = events.publish(7000, 7000, {10, 1}, wake);
    EXPECT_EQ(wake, std::vector<uintptr_t>{7000});
    events.retire(7000, signal, true);
    for (uintptr_t key = 1; key <= 10000; ++key)
        events.unobserve(key, key);
    events.unobserve(7000, 10001);
    EXPECT_EQ(events.size(), 0);
}

TEST(EventTimeline, WrappedSequenceZeroStillHasChannelIdentity)
{
    EventTimeline events;
    std::vector<uintptr_t> wake;
    EXPECT_FALSE(events.publish(1, 1, {}, wake));
    auto signal = events.publish(1, 1, {0, 5}, wake);
    ASSERT_NE(signal, nullptr);
    EXPECT_EQ(events.observe(1, 1, 1, 5), signal);
    EXPECT_FALSE(events.observe(1, 1, 2, 6));
}

TEST(EventTimeline, CompletionAndFailureSettleOnce)
{
    for (int attempt = 0; attempt != 1000; ++attempt) {
        raw::EventPromise signal({10, 1}, 1);
        std::atomic<bool> start{false};
        std::thread producer([&] {
            start.store(true, std::memory_order_release);
            signal.settle(true);
        });
        std::thread failure([&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            signal.settle(false);
        });
        producer.join();
        failure.join();
        const auto outcome = signal.outcome();
        ASSERT_NE(outcome, EventOutcome::pending);
        EXPECT_EQ(signal.orderedOn(1), outcome == EventOutcome::succeeded);
        EXPECT_FALSE(signal.settle(outcome == EventOutcome::failed));
    }
}
}  // namespace
