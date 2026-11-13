// SPDX-License-Identifier: MIT
#include <array>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuPresentTokens.h"

namespace
{
std::array<int, 64> references, states, physicalStates;
uint32_t waits, retainCalls, releaseCalls, refuse, promotions;

switch_gpu_backing_token token(uint32_t queue, uint32_t engine = SWITCH_GPU_ENGINE_NVDEC)
{
	return {1, 1, engine, queue};
}

class PresentTokens : public testing::Test
{
protected:
	switch_gpu_present_tokens held = {};

	void SetUp() override
	{
		references.fill(0);
		states.fill(0);
		physicalStates.fill(0);
		waits = retainCalls = releaseCalls = refuse = promotions = 0;
	}

	switch_gpu_present_pending pending()
	{
		const auto previousReferences = references;
		const auto previousHeld = held;
		const auto previousWaits = waits;
		const auto previousRetains = retainCalls;
		const auto previousReleases = releaseCalls;
		const auto result = switch_gpu_present_tokens_pending(&held);
		EXPECT_EQ(references, previousReferences);
		EXPECT_EQ(waits, previousWaits);
		EXPECT_EQ(retainCalls, previousRetains);
		EXPECT_EQ(releaseCalls, previousReleases);
		EXPECT_EQ(held.count, previousHeld.count);
		for (uint32_t i = 0; i < held.count; ++i) {
			EXPECT_EQ(held.tokens[i].engine, previousHeld.tokens[i].engine);
			EXPECT_EQ(held.tokens[i].queue, previousHeld.tokens[i].queue);
			EXPECT_EQ(held.tokens[i].reset_generation,
				  previousHeld.tokens[i].reset_generation);
			EXPECT_EQ(held.tokens[i].value, previousHeld.tokens[i].value);
		}
		return result;
	}

	void TearDown() override
	{
		switch_gpu_present_tokens_drop(&held);
		for (const auto refs : references)
			EXPECT_EQ(refs, 0);
	}
};
}

extern "C" int switch_gpu_backing_token_retain(const switch_gpu_backing_token *value)
{
	++retainCalls;
	if (value->queue == refuse)
		return -19;
	++references[value->queue];
	return 0;
}

extern "C" void switch_gpu_backing_token_release(const switch_gpu_backing_token *value)
{
	++releaseCalls;
	EXPECT_GT(references[value->queue], 0);
	--references[value->queue];
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *value)
{
	return states[value->queue];
}

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *value,
							int external)
{
	EXPECT_EQ(external, 0);
	return physicalStates[value->queue];
}

extern "C" int switch_gpu_backing_token_wait(const switch_gpu_backing_token *value)
{
	++waits;
	return states[value->queue] = 1;
}

TEST_F(PresentTokens, AdmissionRetainsEveryExactPendingProducerWithoutWaiting)
{
	const auto producers = std::array{token(1), token(2)};
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	EXPECT_EQ(held.count, 2u);
	EXPECT_EQ(waits, 0u);
	EXPECT_EQ(references[1], 1);
	EXPECT_EQ(references[2], 1);
	EXPECT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), 1), -16);
}

TEST_F(PresentTokens, TooManyDependenciesFailBeforeAnyRetain)
{
	const auto producer = token(1);
	EXPECT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 33), -22);
	EXPECT_EQ(switch_gpu_present_tokens_prepare(&held, nullptr, 1), -22);
	EXPECT_EQ(retainCalls, 0u);
}

TEST_F(PresentTokens, PartialRetainFailureUnwindsEarlierTokens)
{
	const auto producers = std::array{token(1), token(2), token(3)};
	refuse = 2;
	EXPECT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()),
		  -19);
	EXPECT_EQ(held.count, 0u);
	EXPECT_EQ(references[1], 0);
	EXPECT_EQ(retainCalls, 2u);
}

// A producer that failed before the present left undefined bytes and nothing to wait for; the present goes on.
TEST_F(PresentTokens, InputFailedBeforeThePresentIsNothingToWaitFor)
{
	const auto producers = std::array{token(1), token(2)};
	states[2] = -5;
	EXPECT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	EXPECT_EQ(held.count, 1u);
	EXPECT_EQ(references[1], 1);
	EXPECT_EQ(references[2], 0);
	switch_gpu_present_tokens_drop(&held);
	EXPECT_EQ(references[1], 0);
}

TEST_F(PresentTokens, ExternalCompletionJoinsLaggingSoftwareRetirementOnlyAfterHardwareFence)
{
	const auto producer = token(1);
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	EXPECT_EQ(waits, 0u);
	EXPECT_EQ(switch_gpu_present_tokens_finish(&held, 0), 0);
	EXPECT_EQ(waits, 1u);
	EXPECT_EQ(held.count, 0u);
	EXPECT_EQ(references[1], 0);
}

// The interrupt pass's end finishes a watched present: nothing there may sleep, since only that pass's own interrupt could
// end the sleep. A producer it finds not complete is a failure, never a wait.
TEST_F(PresentTokens, TheInterruptPassFinishNeverWaits)
{
	const auto producer = token(1, SWITCH_GPU_ENGINE_GM20B);
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	states[1] = 1;
	EXPECT_EQ(switch_gpu_present_tokens_finish_ready(&held, nullptr), 0);
	EXPECT_EQ(held.count, 0u);
	EXPECT_EQ(references[1], 0);
	states[1] = 0;
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	EXPECT_EQ(switch_gpu_present_tokens_finish_ready(&held, nullptr), -5);
	EXPECT_EQ(waits, 0u);
	EXPECT_EQ(references[1], 0);
}

TEST_F(PresentTokens, TerminalFailureAndExistingCopyFailureRemainFailures)
{
	const auto producer = token(1);
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	states[1] = -5;
	EXPECT_EQ(switch_gpu_present_tokens_finish(&held, 0), -5);
	EXPECT_EQ(waits, 0u);
	states[1] = 1;
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	EXPECT_EQ(switch_gpu_present_tokens_finish(&held, -5), -5);
}

namespace
{
int promote(const switch_gpu_backing_token *value)
{
	EXPECT_EQ(value->engine, uint32_t(SWITCH_GPU_ENGINE_GM20B));
	EXPECT_EQ(states[value->queue], 1);
	EXPECT_GT(references[value->queue], 0);
	++promotions;
	return 1;
}
}

TEST_F(PresentTokens, SuccessfulExternalFencePromotesOnlyRetainedGpuSources)
{
	auto producers = std::array{token(1), token(2)};
	producers[0].engine = SWITCH_GPU_ENGINE_GM20B;
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	EXPECT_EQ(switch_gpu_present_tokens_finish_visible(&held, 0, promote), 0);
	EXPECT_EQ(promotions, 1u);
	EXPECT_EQ(held.count, 0u);
}

TEST_F(PresentTokens, FailedPresentDoesNotPublishExternalVisibility)
{
	auto producer = token(1);
	producer.engine = SWITCH_GPU_ENGINE_GM20B;
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	EXPECT_EQ(switch_gpu_present_tokens_finish_visible(&held, -5, promote), -5);
	EXPECT_EQ(promotions, 0u);
}

TEST_F(PresentTokens, AnyFailedSourcePreventsPromotionOfOtherwiseCompletedInputs)
{
	auto producers = std::array{token(1), token(2)};
	producers[0].engine = SWITCH_GPU_ENGINE_GM20B;
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	states[1] = 1;
	states[2] = -5;
	EXPECT_EQ(switch_gpu_present_tokens_finish_visible(&held, 0, promote), -5);
	EXPECT_EQ(promotions, 0u);
}

TEST_F(PresentTokens, TimeoutAttributionHasNoProducerWhenEmptyOrComplete)
{
	EXPECT_EQ(pending().token.engine, 0u);
	const auto producers = std::array{token(1), token(2, SWITCH_GPU_ENGINE_VIC),
					  token(3, SWITCH_GPU_ENGINE_GM20B)};
	states.fill(1);
	physicalStates.fill(1);
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	EXPECT_EQ(pending().token.engine, 0u);
}

TEST_F(PresentTokens, TimeoutAttributionPrefersFirstPhysicallyPendingGpuOverVic)
{
	const auto producers =
		std::array{token(1, SWITCH_GPU_ENGINE_VIC), token(2, SWITCH_GPU_ENGINE_GM20B),
			   token(3, SWITCH_GPU_ENGINE_GM20B)};
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	const auto result = pending();
	EXPECT_EQ(result.token.queue, 2u);
	EXPECT_EQ(result.status, 0);
	EXPECT_EQ(result.physical, 0);
	EXPECT_TRUE(switch_gpu_present_pending_matches(&result, 2, 0));
}

TEST_F(PresentTokens, SemanticRetirementDoesNotHidePhysicallyPendingGpu)
{
	const auto producers =
		std::array{token(1, SWITCH_GPU_ENGINE_GM20B), token(2, SWITCH_GPU_ENGINE_GM20B)};
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	physicalStates[1] = 1;
	EXPECT_EQ(pending().token.queue, 2u);
}

TEST_F(PresentTokens, FailedGpuDoesNotHideRecoverablePendingGpu)
{
	const auto producers =
		std::array{token(1, SWITCH_GPU_ENGINE_GM20B), token(2, SWITCH_GPU_ENGINE_GM20B)};
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	states[1] = -5;
	EXPECT_EQ(pending().token.queue, 2u);
}

TEST_F(PresentTokens, FallbackKeepsFirstFailureWhenNoGpuCanBeRecovered)
{
	const auto producers = std::array{token(1), token(2, SWITCH_GPU_ENGINE_GM20B)};
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, producers.data(), producers.size()), 0);
	states[1] = -5;
	physicalStates.fill(1);
	const auto result = pending();
	EXPECT_EQ(result.token.queue, 1u);
	EXPECT_EQ(result.status, -5);
	EXPECT_EQ(result.physical, 1);
	EXPECT_FALSE(switch_gpu_present_pending_matches(&result, 1, 0));
}

TEST_F(PresentTokens, FailedGpuIsReportedWithoutBeingRecovered)
{
	const auto producer = token(1, SWITCH_GPU_ENGINE_GM20B);
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	states[1] = -5;
	const auto result = pending();
	EXPECT_EQ(result.token.queue, 1u);
	EXPECT_EQ(result.status, -5);
	EXPECT_EQ(result.physical, 0);
	EXPECT_FALSE(switch_gpu_present_pending_matches(&result, 1, 0));
}

TEST_F(PresentTokens, SemanticOnlyPendingGpuIsReportedWithoutBeingRecovered)
{
	const auto producer = token(1, SWITCH_GPU_ENGINE_GM20B);
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	physicalStates[1] = 1;
	const auto result = pending();
	EXPECT_EQ(result.token.queue, 1u);
	EXPECT_EQ(result.status, 0);
	EXPECT_EQ(result.physical, 1);
	EXPECT_FALSE(switch_gpu_present_pending_matches(&result, 1, 0));
}

TEST(PresentPending, RecoveryRequiresTheSameLiveUnfinishedProducer)
{
	const uint64_t channelId = (uint64_t{2} << 32) | 5;
	switch_gpu_present_pending original = {};
	original.token = token(5, SWITCH_GPU_ENGINE_GM20B);
	original.token.reset_generation = 3;
	original.token.value = 17;
	ASSERT_TRUE(switch_gpu_present_pending_matches(&original, channelId, 16));
	EXPECT_FALSE(switch_gpu_present_pending_matches(&original, channelId + 1, 16));
	EXPECT_FALSE(
		switch_gpu_present_pending_matches(&original, channelId + (uint64_t{1} << 32), 16));
	EXPECT_FALSE(switch_gpu_present_pending_matches(&original, channelId, 17));
	EXPECT_FALSE(switch_gpu_present_pending_matches(&original, channelId, 18));
	for (const int state : {-5, 1}) {
		auto changed = original;
		changed.status = state;
		EXPECT_FALSE(switch_gpu_present_pending_matches(&changed, channelId, 16));
		changed = original;
		changed.physical = state;
		EXPECT_FALSE(switch_gpu_present_pending_matches(&changed, channelId, 16));
	}
	auto changed = original;
	changed.token.engine = SWITCH_GPU_ENGINE_VIC;
	EXPECT_FALSE(switch_gpu_present_pending_matches(&changed, channelId, 16));
	changed = original;
	changed.token.value = (uint64_t{1} << 32) + 17;
	EXPECT_FALSE(switch_gpu_present_pending_matches(&changed, channelId, 16));
}

TEST(PresentPending, RecoveryRechecksFenceProgressAcrossSequenceWrap)
{
	switch_gpu_present_pending value = {};
	value.token = token(1, SWITCH_GPU_ENGINE_GM20B);
	value.token.value = 0x10;
	EXPECT_TRUE(switch_gpu_present_pending_matches(&value, 1, 0xfffffff0));
	value.token.value = 0xfffffff0;
	EXPECT_FALSE(switch_gpu_present_pending_matches(&value, 1, 0x10));
}
