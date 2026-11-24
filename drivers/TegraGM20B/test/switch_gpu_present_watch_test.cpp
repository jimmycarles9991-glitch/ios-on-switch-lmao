// SPDX-License-Identifier: MIT
// SwitchGpuPresentWatch.h: a direct present's producer watch, made due by the producer channel's interrupt pass and
// claimed by exactly one of the pass's end, the work item and the offer's own late look.
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuPresentWatch.h"

namespace
{
int completedStatus, externalStatus, semanticStatus;

int completed(const switch_gpu_backing_token *)
{
	return completedStatus;
}

int external(const switch_gpu_backing_token *)
{
	return externalStatus;
}

int semantic(const switch_gpu_backing_token *)
{
	return semanticStatus;
}

constexpr uint64_t kUid = (uint64_t(2) << 32) | 17u;  // reset generation 3, queue 17

class PresentWatch : public testing::Test
{
protected:
	switch_gpu_present_watch watch = {};
	const switch_gpu_backing_token producer = {40, 3, SWITCH_GPU_ENGINE_GM20B, 17};

	void SetUp() override
	{
		completedStatus = 0;
		externalStatus = 0;
		semanticStatus = 0;
		switch_gpu_present_watch_arm(&watch, &producer);
	}
};

TEST(PresentWatchable, UpToFourGm20bProducersOrOneWriterPromise)
{
	const switch_gpu_backing_token gm20b = {1, 1, SWITCH_GPU_ENGINE_GM20B, 1};
	const switch_gpu_backing_token vic = {1, 1, SWITCH_GPU_ENGINE_VIC, 1};
	const switch_gpu_backing_token promise = {9, 0, SWITCH_GPU_ENGINE_PROMISE, 2052};
	const switch_gpu_backing_token two[2] = {gm20b, gm20b};
	const switch_gpu_backing_token four[4] = {gm20b, gm20b, gm20b, gm20b};
	const switch_gpu_backing_token five[5] = {gm20b, gm20b, gm20b, gm20b, gm20b};
	const switch_gpu_backing_token mixed[2] = {gm20b, vic};
	const switch_gpu_backing_token twoPromises[2] = {promise, promise};
	const switch_gpu_backing_token gmAndPromise[2] = {gm20b, promise};
	EXPECT_TRUE(switch_gpu_present_watchable(&gm20b, 1));
	EXPECT_TRUE(switch_gpu_present_watchable(&promise, 1));
	EXPECT_TRUE(switch_gpu_present_watchable(two, 2));
	EXPECT_TRUE(switch_gpu_present_watchable(four, 4));
	EXPECT_FALSE(switch_gpu_present_watchable(five, 5));
	EXPECT_FALSE(switch_gpu_present_watchable(&vic, 1));
	EXPECT_FALSE(switch_gpu_present_watchable(mixed, 2));
	EXPECT_FALSE(switch_gpu_present_watchable(twoPromises, 2));
	EXPECT_FALSE(switch_gpu_present_watchable(gmAndPromise, 2));
	EXPECT_FALSE(switch_gpu_present_watchable(nullptr, 0));
}

TEST_F(PresentWatch, TheProducersOwnChannelReachingItsSeqnoMakesItDue)
{
	EXPECT_TRUE(switch_gpu_present_watch_armed(&watch));
	EXPECT_FALSE(switch_gpu_present_watch_fenced(&watch, kUid, 39));
	EXPECT_FALSE(switch_gpu_present_watch_fenced(&watch, kUid + 1, 40));  // another channel
	EXPECT_FALSE(switch_gpu_present_watch_fenced(&watch, (uint64_t(3) << 32) | 17u,
						     40));  // reset since
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_ARMED));
	EXPECT_TRUE(switch_gpu_present_watch_fenced(&watch, kUid, 41));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_DUE));
	EXPECT_FALSE(switch_gpu_present_watch_fenced(&watch, kUid, 42));  // once
}

TEST_F(PresentWatch, ThePassTakesOnlyAVisibleProducerWithNothingSemanticLeft)
{
	ASSERT_TRUE(switch_gpu_present_watch_fenced(&watch, kUid, 40));
	semanticStatus = 0;
	externalStatus = 1;
	EXPECT_FALSE(switch_gpu_present_watch_take_visible(&watch, semantic, external));
	semanticStatus = 1;
	externalStatus = 0;
	EXPECT_FALSE(switch_gpu_present_watch_take_visible(&watch, semantic, external));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch),
		  uint32_t(SWITCH_GPU_PRESENT_WATCH_DUE));  // left for the work item
	externalStatus = 1;
	EXPECT_TRUE(switch_gpu_present_watch_take_visible(&watch, semantic, external));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch),
		  uint32_t(SWITCH_GPU_PRESENT_WATCH_CLAIMED));
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_PENDING);  // claimed: nobody else delivers it
	EXPECT_TRUE(switch_gpu_present_watch_armed(&watch));
	switch_gpu_present_watch_reset(&watch);
	EXPECT_FALSE(switch_gpu_present_watch_armed(&watch));
}

TEST_F(PresentWatch, ThePassDoesNotTakeAnArmedWatch)
{
	semanticStatus = externalStatus = 1;
	EXPECT_FALSE(switch_gpu_present_watch_take_visible(&watch, semantic, external));
}

TEST_F(PresentWatch, AClaimNamesWhatTheProducerCameTo)
{
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_PENDING);
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_ARMED));
	completedStatus = 1;
	externalStatus = 0;
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_HIDDEN);
	switch_gpu_present_watch_arm(&watch, &producer);
	externalStatus = 1;
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_VISIBLE);
	switch_gpu_present_watch_arm(&watch, &producer);
	completedStatus = -5;
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_FAILED);
	switch_gpu_present_watch_arm(&watch, &producer);
	completedStatus = 1;
	externalStatus = -5;
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_FAILED);
}

TEST_F(PresentWatch, ADueWatchIsClaimedOnceByTheWorkItem)
{
	ASSERT_TRUE(switch_gpu_present_watch_fenced(&watch, kUid, 40));
	completedStatus = externalStatus = 1;
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_VISIBLE);
	semanticStatus = 1;
	EXPECT_FALSE(switch_gpu_present_watch_take_visible(&watch, semantic, external));
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_PENDING);
}

// A watch waiting for a promise: no pass looks at it; the job that keeps the promise becomes its producer, claimed while
// it is handed over so neither the pass nor the work item acts on a half-written token.
TEST(PromiseWatch, TheKeepingJobBecomesTheProducerAndOnlyItsOwnPromiseConverts)
{
	switch_gpu_present_watch watch = {};
	const switch_gpu_backing_token promise = {9, 0, SWITCH_GPU_ENGINE_PROMISE, 2052};
	const switch_gpu_backing_token job = {40, 3, SWITCH_GPU_ENGINE_GM20B, 17};
	switch_gpu_present_watch_promise(&watch, &promise);
	EXPECT_TRUE(switch_gpu_present_watch_armed(&watch));
	EXPECT_FALSE(switch_gpu_present_watch_fenced(&watch, 2052, 9));
	EXPECT_TRUE(switch_gpu_present_watch_promised(&watch, 2052, 9));
	EXPECT_FALSE(switch_gpu_present_watch_kept(&watch, 2052, 8, &job));
	EXPECT_FALSE(switch_gpu_present_watch_kept(&watch, 2056, 9, &job));
	ASSERT_TRUE(switch_gpu_present_watch_kept(&watch, 2052, 9, &job));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch),
		  uint32_t(SWITCH_GPU_PRESENT_WATCH_CLAIMED));
	completedStatus = externalStatus = 1;
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_PENDING);
	switch_gpu_present_watch_rearm(&watch);
	EXPECT_EQ(watch.tokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_GM20B));
	EXPECT_TRUE(switch_gpu_present_watch_fenced(&watch, kUid, 40));
}

// The work item claimed the promised watch first (a CPU release, the deadline): the late job's conversion finds it gone.
TEST(PromiseWatch, AClaimedPromiseWatchIsNotConverted)
{
	switch_gpu_present_watch watch = {};
	const switch_gpu_backing_token promise = {9, 0, SWITCH_GPU_ENGINE_PROMISE, 2052};
	const switch_gpu_backing_token job = {40, 3, SWITCH_GPU_ENGINE_GM20B, 17};
	switch_gpu_present_watch_promise(&watch, &promise);
	completedStatus = -5;
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_FAILED);
	EXPECT_FALSE(switch_gpu_present_watch_kept(&watch, 2052, 9, &job));
	EXPECT_EQ(watch.tokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
}

TEST(MultiPlaneWatch, StaysArmedUntilLastOfFourProducersFences)
{
	switch_gpu_present_watch watch = {};
	const switch_gpu_backing_token tokens[4] = {
		{10, 1, SWITCH_GPU_ENGINE_GM20B, 1},
		{20, 1, SWITCH_GPU_ENGINE_GM20B, 2},
		{30, 2, SWITCH_GPU_ENGINE_GM20B, 3},
		{40, 1, SWITCH_GPU_ENGINE_GM20B, 1},
	};
	switch_gpu_present_watch_arm_many(&watch, tokens, 4);
	EXPECT_FALSE(switch_gpu_present_watch_fenced(&watch, 1, 10));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_ARMED));
	EXPECT_FALSE(switch_gpu_present_watch_fenced(&watch, (uint64_t(1) << 32) | 3u, 30));
	EXPECT_FALSE(switch_gpu_present_watch_fenced(&watch, 2, 20));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_ARMED));
	EXPECT_TRUE(switch_gpu_present_watch_fenced(&watch, 1, 40));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_DUE));
	semanticStatus = externalStatus = 1;
	EXPECT_TRUE(switch_gpu_present_watch_take_visible(&watch, semantic, external));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch),
		  uint32_t(SWITCH_GPU_PRESENT_WATCH_CLAIMED));
}

TEST(MultiPlaneWatch, CheckClearsPreCompletedProducerSoRemainingPassMakesWatchDue)
{
	switch_gpu_present_watch watch = {};
	const switch_gpu_backing_token tokens[2] = {
		{10, 1, SWITCH_GPU_ENGINE_GM20B, 1},
		{20, 1, SWITCH_GPU_ENGINE_GM20B, 2},
	};
	const uint64_t gen = switch_gpu_present_watch_arm_many(&watch, tokens, 2);
	auto onlyFirstDone = [](const switch_gpu_backing_token *t) -> int {
		return t->queue == 1 ? 1 : 0;
	};
	EXPECT_FALSE(switch_gpu_present_watch_check(&watch, gen, tokens, 2, onlyFirstDone));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_ARMED));
	EXPECT_TRUE(switch_gpu_present_watch_fenced(&watch, 2, 20));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_DUE));
}

TEST(MultiPlaneWatch, CheckTransitionsToDueWhenAllAlreadyDone)
{
	switch_gpu_present_watch watch = {};
	const switch_gpu_backing_token tokens[2] = {
		{10, 1, SWITCH_GPU_ENGINE_GM20B, 1},
		{20, 1, SWITCH_GPU_ENGINE_GM20B, 2},
	};
	const uint64_t gen = switch_gpu_present_watch_arm_many(&watch, tokens, 2);
	completedStatus = externalStatus = 1;
	EXPECT_TRUE(switch_gpu_present_watch_check(&watch, gen, tokens, 2, completed));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_DUE));
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, completed, external),
		  SWITCH_GPU_PRESENT_WATCH_VISIBLE);
}

TEST(MultiPlaneWatch, HiddenOrFailedTokenInSetClaimsRightVerdict)
{
	switch_gpu_present_watch watch = {};
	const switch_gpu_backing_token tokens[2] = {
		{10, 1, SWITCH_GPU_ENGINE_GM20B, 1},
		{20, 1, SWITCH_GPU_ENGINE_GM20B, 2},
	};
	switch_gpu_present_watch_arm_many(&watch, tokens, 2);
	ASSERT_FALSE(switch_gpu_present_watch_fenced(&watch, 1, 10));
	ASSERT_TRUE(switch_gpu_present_watch_fenced(&watch, 2, 20));
	auto allDone = [](const switch_gpu_backing_token *) -> int {
		return 1;
	};
	auto secondHidden = [](const switch_gpu_backing_token *t) -> int {
		return t->queue == 1 ? 1 : 0;
	};
	EXPECT_FALSE(switch_gpu_present_watch_take_visible(&watch, allDone, secondHidden));
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, allDone, secondHidden),
		  SWITCH_GPU_PRESENT_WATCH_HIDDEN);

	switch_gpu_present_watch_reset(&watch);
	switch_gpu_present_watch_arm_many(&watch, tokens, 2);
	auto firstFailedSecondPending = [](const switch_gpu_backing_token *t) -> int {
		return t->queue == 1 ? -5 : 0;
	};
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, firstFailedSecondPending, allDone),
		  SWITCH_GPU_PRESENT_WATCH_FAILED);
}

TEST(PromiseWatch, RearmAdvancesGenerationPreventingStalePromiseClaimOrCheck)
{
	static switch_gpu_present_watch *activeWatch = nullptr;
	static switch_gpu_backing_token keepingJob = {};
	switch_gpu_present_watch watch = {};
	const switch_gpu_backing_token promise = {9, 0, SWITCH_GPU_ENGINE_PROMISE, 2052};
	const switch_gpu_backing_token job = {40, 3, SWITCH_GPU_ENGINE_GM20B, 17};

	const uint64_t promiseGen = switch_gpu_present_watch_promise(&watch, &promise);
	activeWatch = &watch;
	keepingJob = job;

	auto raceKeepOnCompleted = [](const switch_gpu_backing_token *) -> int {
		EXPECT_TRUE(switch_gpu_present_watch_kept(activeWatch, 2052, 9, &keepingJob));
		switch_gpu_present_watch_rearm(activeWatch);
		return 1;
	};
	auto alwaysVisible = [](const switch_gpu_backing_token *) -> int {
		return 1;
	};

	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, raceKeepOnCompleted, alwaysVisible),
		  SWITCH_GPU_PRESENT_WATCH_PENDING);
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_ARMED));
	EXPECT_GT(watch.ctrl >> 8, promiseGen);
	EXPECT_FALSE(
		switch_gpu_present_watch_check(&watch, promiseGen, &promise, 1, alwaysVisible));
	EXPECT_EQ(switch_gpu_present_watch_state(&watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_ARMED));

	auto hiddenJob = [](const switch_gpu_backing_token *) -> int {
		return 0;
	};
	ASSERT_TRUE(switch_gpu_present_watch_fenced(&watch, kUid, 40));
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, alwaysVisible, hiddenJob),
		  SWITCH_GPU_PRESENT_WATCH_HIDDEN);

	switch_gpu_present_watch_reset(&watch);
	switch_gpu_present_watch_promise(&watch, &promise);
	EXPECT_EQ(switch_gpu_present_watch_claim(&watch, alwaysVisible, alwaysVisible),
		  SWITCH_GPU_PRESENT_WATCH_HIDDEN);
}
}  // namespace
