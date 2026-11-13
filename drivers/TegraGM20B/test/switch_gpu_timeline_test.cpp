// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "switch_gpu_timeline_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuTimeline.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"

struct switch_gpu_event_waits {
	unsigned refs = 1;
	int result = 0;
};

namespace
{
switch_gpu_backing_engine operations{};
switch_gpu_backing_visibility visibility{};
unsigned promotions;
std::atomic<unsigned> fenceRefs, notifications;
std::atomic<unsigned> waits;
uint64_t nextId;
volatile uint32_t *waitFence;
}

extern "C" int switch_gpu_backing_register_engine(uint32_t engine,
						  const switch_gpu_backing_engine *ops)
{
	if (engine != SWITCH_GPU_ENGINE_GM20B)
		return -22;
	operations = *ops;
	return 0;
}

extern "C" int switch_gpu_backing_register_visibility(uint32_t,
						      const switch_gpu_backing_visibility *ops)
{
	visibility = *ops;
	return 0;
}

extern "C" int switch_gpu_backing_register_semantics(uint32_t, const switch_gpu_backing_semantics *)
{
	return 0;
}

extern "C" void switch_gpu_event_waits_retain(switch_gpu_event_waits *batch)
{
	++batch->refs;
}

extern "C" void switch_gpu_event_waits_release(switch_gpu_event_waits **batch)
{
	if (*batch) {
		--(*batch)->refs;
		*batch = nullptr;
	}
}

extern "C" int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *batch)
{
	return batch->result;
}

extern "C" int switch_gpu_event_waits_wait(switch_gpu_event_waits *batch, uint64_t)
{
	return batch->result;
}

extern "C" int switch_gpu_bridge_wait_external(const switch_gpu_backing_token *token)
{
	++promotions;
	if (waitFence)
		__atomic_store_n(waitFence, uint32_t(token->value), __ATOMIC_RELEASE);
	return switch_gpu_timeline_promote(token);
}

extern "C" void switch_gpu_fence_hold(const volatile uint32_t *)
{
	++fenceRefs;
}

extern "C" void switch_gpu_fence_put(const volatile uint32_t *)
{
	--fenceRefs;
}

extern "C" void switch_gpu_backing_completed()
{
	++notifications;
}

extern "C" void tgm_writer_wait(uint32_t, uint32_t value)
{
	switchTimelineMayBlock();
	++waits;
	if (waitFence)
		__atomic_store_n(waitFence, value, __ATOMIC_RELEASE);
}

namespace
{
class Timeline : public testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_EQ(switch_gpu_timeline_init(), 0);
		ASSERT_EQ(fenceRefs.load(), 0u);
		notifications = waits = 0;
		promotions = 0;
		waitFence = nullptr;
	}

	void TearDown() override
	{
		EXPECT_EQ(fenceRefs.load(), 0u);
	}
};

switch_gpu_backing_token token(switch_gpu_timeline *timeline, uint32_t value)
{
	switch_gpu_backing_token result{};
	switch_gpu_timeline_token(timeline, value, &result);
	return result;
}
}

TEST_F(Timeline, FailurePublicationDoesNotBlockOrAllocateInEventContext)
{
	volatile uint32_t fence = 4;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	ASSERT_NE(timeline, nullptr);
	auto completed = token(timeline, 4), pending = token(timeline, 5);
	ASSERT_EQ(operations.retain_token(&pending), 0);
	switchTimelineAtomicContext = true;
	switch_gpu_timeline_failed(timeline, 4);
	switchTimelineAtomicContext = false;
	__atomic_store_n(&fence, 100, __ATOMIC_RELEASE);
	EXPECT_EQ(operations.status(&completed), 1);
	EXPECT_EQ(operations.status(&pending), -5);
	EXPECT_EQ(notifications.load(), 1u);
	switch_gpu_timeline_close(timeline);
	EXPECT_EQ(fenceRefs.load(), 1u);
	EXPECT_EQ(operations.status(&pending), -5);
	operations.release_token(&pending);
}

TEST_F(Timeline, FirstFailureIncludingZeroCannotBecomeSyntheticSuccess)
{
	volatile uint32_t fence = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	auto pending = token(timeline, 1);
	switch_gpu_timeline_failed(timeline, 0);
	switch_gpu_timeline_failed(timeline, 100);
	__atomic_store_n(&fence, 100, __ATOMIC_RELEASE);
	EXPECT_EQ(operations.status(&pending), -5);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, ClosedTimelinePreservesTrueCompletionAndPinsFenceForRetainedTokens)
{
	volatile uint32_t fence = 8;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	auto completed = token(timeline, 8), pending = token(timeline, 9);
	ASSERT_EQ(operations.retain_token(&pending), 0);
	switch_gpu_timeline_close(timeline);
	EXPECT_EQ(fenceRefs.load(), 1u);
	__atomic_store_n(&fence, 9, __ATOMIC_RELEASE);
	EXPECT_EQ(operations.status(&completed), 1);
	EXPECT_EQ(operations.status(&pending), -5);
	EXPECT_EQ(operations.wait(&pending), -5);
	EXPECT_EQ(waits.load(), 0u);
	operations.release_token(&pending);
	EXPECT_EQ(operations.status(&pending), -19);
}

TEST_F(Timeline, PendingWaitUsesTheEngineAndCompletedWaitDoesNot)
{
	volatile uint32_t fence = 1;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	auto pending = token(timeline, 2);
	waitFence = &fence;
	EXPECT_EQ(operations.wait(&pending), 1);
	EXPECT_EQ(waits.load(), 1u);
	EXPECT_EQ(operations.wait(&pending), 1);
	EXPECT_EQ(waits.load(), 1u);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, ConcurrentFailureAndAbandonedFenceNeverReportSuccess)
{
	for (unsigned round = 0; round < 1000; ++round) {
		volatile uint32_t fence = 1;
		auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
		auto pending = token(timeline, 2);
		std::atomic<bool> start{false}, finished{false};
		std::thread recovery([&] {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			switchTimelineAtomicContext = true;
			switch_gpu_timeline_failed(timeline, 1);
			__atomic_store_n(&fence, 2, __ATOMIC_RELEASE);
			switchTimelineAtomicContext = false;
			finished.store(true, std::memory_order_release);
		});
		start.store(true, std::memory_order_release);
		do {
			EXPECT_NE(operations.status(&pending), 1);
		} while (!finished.load(std::memory_order_acquire));
		recovery.join();
		EXPECT_EQ(operations.status(&pending), -5);
		switch_gpu_timeline_close(timeline);
	}
}

TEST_F(Timeline, CompletionComparisonPreservesThirtyTwoBitWrap)
{
	volatile uint32_t fence = 1;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	auto before = token(timeline, UINT32_MAX), after = token(timeline, 2);
	switch_gpu_timeline_failed(timeline, 1);
	EXPECT_EQ(operations.status(&before), 1);
	EXPECT_EQ(operations.status(&after), -5);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, InternalCompletionDoesNotPromiseExternalVisibility)
{
	volatile uint32_t fence = 0, visible = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_visibility(timeline, &visible);
	auto done = token(timeline, 1);
	fence = 1;
	EXPECT_EQ(operations.status(&done), 1);
	EXPECT_EQ(visibility.status(&done), 0);
	visible = 1;
	EXPECT_EQ(visibility.status(&done), 1);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, ExternalFenceCoversAllPriorInternalWork)
{
	volatile uint32_t fence = 0, visible = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_visibility(timeline, &visible);
	fence = 3;
	visible = 2;
	auto first = token(timeline, 1), next = token(timeline, 3);
	EXPECT_EQ(visibility.status(&first), 1);
	EXPECT_EQ(visibility.status(&next), 0);
	EXPECT_EQ(visibility.wait(&next), 1);
	EXPECT_EQ(promotions, 1u);
	EXPECT_EQ(visibility.wait(&next), 1);
	EXPECT_EQ(promotions, 1u);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, ClosedInternalTimelineCanBePromotedWithoutAReusedFenceWord)
{
	volatile uint32_t fence = 0, visible = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_visibility(timeline, &visible);
	auto done = token(timeline, 1);
	ASSERT_EQ(operations.retain_token(&done), 0);
	fence = 1;
	switch_gpu_timeline_close(timeline);
	visible = 100;
	EXPECT_EQ(visibility.status(&done), 0);
	EXPECT_EQ(visibility.wait(&done), 1);
	operations.release_token(&done);
}

TEST_F(Timeline, FailedPendingWorkCannotBePromoted)
{
	volatile uint32_t fence = 1, visible = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_visibility(timeline, &visible);
	auto failed = token(timeline, 2), completed = token(timeline, 1);
	switchTimelineAtomicContext = true;
	switch_gpu_timeline_failed(timeline, 1);
	switchTimelineAtomicContext = false;
	fence = 2;
	EXPECT_EQ(visibility.wait(&failed), -5);
	EXPECT_EQ(promotions, 0u);
	EXPECT_EQ(switch_gpu_timeline_promote(&failed), -5);
	EXPECT_EQ(visibility.status(&completed), 0);
	EXPECT_EQ(visibility.wait(&completed), 1);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, VisibleHighwaterPreservesWrap)
{
	volatile uint32_t fence = UINT32_MAX - 1, visible = UINT32_MAX - 1;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_visibility(timeline, &visible);
	fence = 1;
	visible = 0;
	auto before = token(timeline, UINT32_MAX), after = token(timeline, 1);
	EXPECT_EQ(visibility.status(&before), 1);
	EXPECT_EQ(visibility.status(&after), 0);
	EXPECT_EQ(switch_gpu_timeline_promote(&after), 1);
	EXPECT_EQ(visibility.status(&after), 1);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, SemanticPrefixGatesBackingAndMappedCompletionAfterPhysicalFence)
{
	volatile uint32_t fence = 4;
	TegraGM20BAbiFenceSemantic mapped = {};
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_semantic_visibility(timeline, &mapped);
	switch_gpu_event_waits authority;
	auto at = token(timeline, 5), later = token(timeline, 6), earlier = token(timeline, 4);
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&at, &authority, &node), 0);
	EXPECT_EQ(mapped.epoch & 1, 0u);
	EXPECT_EQ(mapped.flags, uint32_t(TegraGM20BFenceSemanticPending));
	EXPECT_EQ(mapped.pending_first, 5u);
	fence = 6;
	EXPECT_EQ(switch_gpu_timeline_physical_status(&later, 0), 1);
	EXPECT_EQ(operations.status(&later), 0);
	EXPECT_EQ(visibility.status(&later), 0);
	EXPECT_EQ(operations.status(&earlier), 1);
	switch_gpu_event_waits *captured = nullptr;
	EXPECT_EQ(switch_gpu_timeline_semantic_snapshot(&later, &captured), 1);
	EXPECT_EQ(captured, &authority);
	switch_gpu_event_waits_release(&captured);
	switch_gpu_timeline_semantic_finish(&node, 0);
	EXPECT_EQ(mapped.flags, 0u);
	EXPECT_EQ(operations.status(&later), 1);
	EXPECT_EQ(authority.refs, 1u);
	EXPECT_FALSE(switch_gpu_timeline_has_semantics());
	switch_gpu_timeline_close(timeline);
}

// Job 9's event wait failed: job 9 failed. Jobs 10-12 on the channel did not wait for that event and completed.
TEST_F(Timeline, SemanticFailureFailsOnlyTheJobThatWaited)
{
	volatile uint32_t fence = 8;
	TegraGM20BAbiFenceSemantic mapped = {};
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_semantic_visibility(timeline, &mapped);
	switch_gpu_event_waits authority;
	auto at = token(timeline, 9), later = token(timeline, 12), earlier = token(timeline, 8);
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&at, &authority, &node), 0);
	fence = 12;
	authority.result = -5;
	EXPECT_EQ(operations.status(&at), -5);
	EXPECT_EQ(operations.status(&later), 1);
	switch_gpu_timeline_semantic_finish(&node, -5);
	EXPECT_EQ(operations.status(&at), -5);
	EXPECT_EQ(visibility.status(&at), -5);
	EXPECT_EQ(operations.status(&later), 1);
	EXPECT_EQ(visibility.status(&later), 1);
	EXPECT_EQ(operations.status(&earlier), 1);
	EXPECT_EQ(mapped.flags, uint32_t(TegraGM20BFenceSemanticFailed));
	EXPECT_EQ(mapped.failed_first, 9u);
	switch_gpu_event_waits *captured = nullptr;
	EXPECT_EQ(switch_gpu_timeline_semantic_snapshot(&at, &captured), -5);
	EXPECT_EQ(switch_gpu_timeline_semantic_snapshot(&later, &captured), 0);
	EXPECT_EQ(captured, nullptr);
	switch_gpu_timeline_close(timeline);
	EXPECT_FALSE(switch_gpu_timeline_has_semantics());
}

// A later job with event waits of its own succeeds when they do, whatever an earlier job's waits did.
TEST_F(Timeline, LaterJobWithItsOwnWaitsIsJudgedByThemAlone)
{
	volatile uint32_t fence = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_event_waits failed, fine;
	failed.result = -5;
	fine.result = 1;
	auto first = token(timeline, 1), second = token(timeline, 2);
	switch_gpu_timeline_semantic *a = nullptr, *b = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&first, &failed, &a), 0);
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&second, &fine, &b), 0);
	fence = 2;
	EXPECT_EQ(operations.status(&first), -5);
	EXPECT_EQ(operations.status(&second), 1);
	switch_gpu_timeline_semantic_finish(&a, -5);
	switch_gpu_timeline_semantic_finish(&b, 0);
	EXPECT_EQ(operations.status(&first), -5);
	EXPECT_EQ(operations.status(&second), 1);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, AbortedUnpublishedSemanticReservationDoesNotPoisonReusedSequence)
{
	volatile uint32_t fence = UINT32_MAX;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_event_waits authority;
	auto at = token(timeline, 0);
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&at, &authority, &node), 0);
	switch_gpu_timeline_semantic_finish(&node, -125);
	fence = 0;
	EXPECT_EQ(operations.status(&at), 1);
	EXPECT_FALSE(switch_gpu_timeline_has_semantics());
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, LateSemanticCompletionCannotOverwriteRecycledPublicFenceSlot)
{
	volatile uint32_t fence = 0;
	TegraGM20BAbiFenceSemantic mapped = {};
	auto *old = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_semantic_visibility(old, &mapped);
	switch_gpu_event_waits authority;
	auto at = token(old, 1);
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&at, &authority, &node), 0);
	switch_gpu_timeline_close(old);
	auto *fresh = switch_gpu_timeline_create(++nextId, &fence);
	switch_gpu_timeline_semantic_visibility(fresh, &mapped);
	const uint32_t freshEpoch = mapped.epoch;
	switch_gpu_timeline_semantic_finish(&node, -5);
	EXPECT_EQ(mapped.epoch, freshEpoch);
	EXPECT_EQ(mapped.flags, 0u);
	switch_gpu_timeline_close(fresh);
}

TEST_F(Timeline, EarlierSemanticFailureCannotRetireLaterPhysicalWork)
{
	volatile uint32_t fence = 4;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	auto first = token(timeline, 4), later = token(timeline, 5);
	switch_gpu_event_waits authority;
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&first, &authority, &node), 0);
	switch_gpu_timeline_semantic_finish(&node, -5);
	EXPECT_EQ(operations.status(&first), -5);
	EXPECT_EQ(operations.status(&later), 0);
	EXPECT_EQ(visibility.status(&later), 0);
	waitFence = &fence;
	EXPECT_EQ(visibility.wait(&later), 1);
	EXPECT_EQ(waits.load(), 1u);
	EXPECT_EQ(fence, 5u);
	EXPECT_EQ(operations.status(&first), -5);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, SuccessfulAuthorityWakeDoesNotRequireItsDeferredCallbackToRunFirst)
{
	volatile uint32_t fence = 4;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	auto current = token(timeline, 4);
	switch_gpu_event_waits authority;
	authority.result = 1;
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&current, &authority, &node), 0);
	EXPECT_EQ(operations.status(&current), 1);
	EXPECT_EQ(switch_gpu_timeline_promote(&current), 1);
	EXPECT_EQ(operations.wait(&current), 1);
	switch_gpu_timeline_semantic_finish(&node, 0);
	EXPECT_EQ(operations.status(&current), 1);
	switch_gpu_timeline_close(timeline);
}
