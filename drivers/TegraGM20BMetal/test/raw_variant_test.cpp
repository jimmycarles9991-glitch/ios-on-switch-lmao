// SPDX-License-Identifier: MIT
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "../raw/runtime/raw_variant.h"
#include "bazel/testing/source_text.h"
#include <memory>
#include <thread>

namespace {

TEST(Variant, PendingObserversWakeExactlyOnceAndSeePublishedState)
{
    raw::Variant<int> variant;
    int pipeline = 17;
    unsigned wakes = 0;
    EXPECT_EQ(variant.observe([&] {
        EXPECT_EQ(variant.value(), &pipeline);
        EXPECT_EQ(variant.observe({}), raw::Variant<int>::State::Ready);
        ++wakes;
    }),
              raw::Variant<int>::State::Pending);
    EXPECT_EQ(variant.value(), nullptr);
    EXPECT_TRUE(variant.publish(&pipeline));
    EXPECT_FALSE(variant.publish(nullptr));
    EXPECT_EQ(variant.observe([&] {
        ++wakes;
    }),
              raw::Variant<int>::State::Ready);
    EXPECT_EQ(wakes, 1u);
    EXPECT_EQ(variant.value(), &pipeline);
}

TEST(Variant, RefusalIsTerminalAndCannotExposeAPartialResult)
{
    raw::Variant<int> variant;
    unsigned wakes = 0;
    variant.observe([&] {
        ++wakes;
    });
    EXPECT_TRUE(variant.publish(nullptr));
    int pipeline = 9;
    EXPECT_FALSE(variant.publish(&pipeline));
    EXPECT_EQ(variant.state(), raw::Variant<int>::State::Refused);
    EXPECT_EQ(variant.value(), nullptr);
    EXPECT_EQ(wakes, 1u);
}

TEST(Variant, CompletionRacingRegistrationEitherWakesOrReturnsReady)
{
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        raw::Variant<int> variant;
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
        EXPECT_EQ(wakes, observed == raw::Variant<int>::State::Pending ? 1u : 0u);
        EXPECT_EQ(*variant.value(), 42);
    }
}

TEST(Variant, PendingProducerRetainsCellAfterRecordingOwnerReleasesIt)
{
    auto owner = std::make_shared<raw::Variant<int>>();
    std::weak_ptr<raw::Variant<int>> weak = owner;
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

// A variant no compiler made fails that draw's command buffer alone; the next draw that needs it asks afresh: no count
TEST(VariantAsks, TheDrawThatMeetsAFailedVariantAsksItAfresh)
{
    const std::string cubic = switch_ios::test::SourceText("../raw/runtime/RawMetalCubic.mm");
    const std::string made = switch_ios::test::Body(cubic, "static raw::Variant<raw::Pipeline> *renderVariant(");
    EXPECT_THAT(made, testing::HasSubstr("found->second->state() != raw::Variant<raw::Pipeline>::State::Failed"));
    EXPECT_THAT(made, testing::Not(testing::HasSubstr("_variantAsks")));
    EXPECT_THAT(made, testing::HasSubstr("p->_replacedVariants.push_back(std::move(found->second));"));
    EXPECT_THAT(made, testing::HasSubstr("completion->askVariant("));
}

}  // namespace
