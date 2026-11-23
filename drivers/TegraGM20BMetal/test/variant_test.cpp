// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../variant.h"
#include <memory>
#include <thread>

namespace {

TEST(Variant, PendingObserversWakeExactlyOnceAndSeePublishedState)
{
    gm20b::Variant<int> variant;
    int pipeline = 17;
    unsigned wakes = 0;
    EXPECT_EQ(variant.observe([&] {
        EXPECT_EQ(variant.value(), &pipeline);
        EXPECT_EQ(variant.observe({}), gm20b::Variant<int>::State::Ready);
        ++wakes;
    }),
              gm20b::Variant<int>::State::Pending);
    EXPECT_EQ(variant.value(), nullptr);
    EXPECT_TRUE(variant.publish(&pipeline));
    EXPECT_FALSE(variant.publish(nullptr));
    EXPECT_EQ(variant.observe([&] {
        ++wakes;
    }),
              gm20b::Variant<int>::State::Ready);
    EXPECT_EQ(wakes, 1u);
    EXPECT_EQ(variant.value(), &pipeline);
}

TEST(Variant, RefusalIsTerminalAndCannotExposeAPartialResult)
{
    gm20b::Variant<int> variant;
    unsigned wakes = 0;
    variant.observe([&] {
        ++wakes;
    });
    EXPECT_TRUE(variant.publish(nullptr));
    int pipeline = 9;
    EXPECT_FALSE(variant.publish(&pipeline));
    EXPECT_EQ(variant.state(), gm20b::Variant<int>::State::Refused);
    EXPECT_EQ(variant.value(), nullptr);
    EXPECT_EQ(wakes, 1u);
}

TEST(Variant, CompletionRacingRegistrationEitherWakesOrReturnsReady)
{
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        gm20b::Variant<int> variant;
        int pipeline = 0;
        unsigned wakes = 0;
        std::thread producer([&] {
            pipeline = 42;
            variant.publish(&pipeline);
        });
        const auto observed = variant.observe([&] {
            EXPECT_EQ(*variant.value(), 42);
            ++wakes;
        });
        producer.join();
        EXPECT_EQ(wakes, observed == gm20b::Variant<int>::State::Pending ? 1u : 0u);
        EXPECT_EQ(*variant.value(), 42);
    }
}

TEST(Variant, PendingProducerRetainsCellAfterRecordingOwnerReleasesIt)
{
    auto owner = std::make_shared<gm20b::Variant<int>>();
    std::weak_ptr<gm20b::Variant<int>> weak = owner;
    unsigned wakes = 0;
    owner->observe([&] {
        ++wakes;
    });
    int pipeline = 5;
    std::function<void()> complete = [held = owner, &pipeline] {
        held->publish(&pipeline);
    };
    owner.reset();
    EXPECT_FALSE(weak.expired());
    complete();
    EXPECT_EQ(wakes, 1u);
    complete = {};
    EXPECT_TRUE(weak.expired());
}

}  // namespace
