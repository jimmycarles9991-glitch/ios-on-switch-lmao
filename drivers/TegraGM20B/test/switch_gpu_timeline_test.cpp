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
	int (*onCachedStatus)(switch_gpu_event_waits *) = nullptr;
	int (*onWait)(switch_gpu_event_waits *) = nullptr;
	unsigned waitCalls = 0;
};

namespace
{
switch_gpu_backing_engine operations{};
switch_gpu_backing_visibility visibility{};
switch_gpu_backing_timeline_refs refs{};
switch_gpu_backing_failure failure;
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

extern "C" int
switch_gpu_backing_register_timeline_refs(uint32_t engine,
					  const switch_gpu_backing_timeline_refs *ops)
{
	if (engine != SWITCH_GPU_ENGINE_GM20B)
		return -22;
	refs = *ops;
	return 0;
}

extern "C" int switch_gpu_backing_register_failure(uint32_t engine, switch_gpu_backing_failure op)
{
	if (engine != SWITCH_GPU_ENGINE_GM20B)
		return -22;
	failure = op;
	return 0;
}

extern "C" void switch_gpu_event_waits_retain(switch_gpu_event_waits *batch)
{
	__atomic_add_fetch(&batch->refs, 1u, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_event_waits_release(switch_gpu_event_waits **batch)
{
	if (*batch) {
		__atomic_sub_fetch(&(*batch)->refs, 1u, __ATOMIC_ACQ_REL);
		*batch = nullptr;
	}
}

extern "C" int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *batch)
{
	auto *mutableBatch = const_cast<switch_gpu_event_waits *>(batch);
	if (mutableBatch->onCachedStatus)
		return mutableBatch->onCachedStatus(mutableBatch);
	return __atomic_load_n(&batch->result, __ATOMIC_ACQUIRE);
}

extern "C" int switch_gpu_event_waits_wait(switch_gpu_event_waits *batch, uint64_t)
{
	__atomic_add_fetch(&batch->waitCalls, 1u, __ATOMIC_RELAXED);
	if (batch->onWait)
		return batch->onWait(batch);
	return __atomic_load_n(&batch->result, __ATOMIC_ACQUIRE);
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
	switch_gpu_timeline_failed(timeline, 4, 0);
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

// Why a timeline's unfinished tokens failed: a compositor channel's kill is the kernel's own failure; an app channel's
// kill (gpu.recover.kill_one's) and a teardown end that client's frame. A close after a kill keeps the kill's why, and
// work the channel finished before either is no failure.
TEST_F(Timeline, AKillOrTeardownTellsWhyItsUnfinishedTokensFailed)
{
	ASSERT_NE(failure, nullptr);
	const int why[3] = {SWITCH_GPU_BACKING_WRITER_FAILED, SWITCH_GPU_BACKING_WRITER_GONE,
			    SWITCH_GPU_BACKING_WRITER_GONE};
	for (int i = 0; i < 3; ++i) {
		volatile uint32_t fence = 4;
		auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
		ASSERT_NE(timeline, nullptr);
		const auto finished = token(timeline, 4), unfinished = token(timeline, 5);
		ASSERT_EQ(operations.retain_token(&finished), 0);
		ASSERT_EQ(operations.retain_token(&unfinished), 0);
		if (i < 2)
			switch_gpu_timeline_failed(timeline, 4, i == 0);
		switch_gpu_timeline_close(timeline);
		EXPECT_EQ(operations.status(&finished), 1);
		EXPECT_EQ(operations.status(&unfinished), -5);
		EXPECT_EQ(failure(&unfinished), why[i]) << "timeline " << i;
		operations.release_token(&finished);
		operations.release_token(&unfinished);
	}
}

// A job's records take their references many at once, and read their tokens through the timeline's handle with no lock
// while it has no semantic wait or failure; with one they read what the engine's status reads.
TEST_F(Timeline, HeldReferencesReadTheEngineStatusWithoutItsLock)
{
	volatile uint32_t fence = 3;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	const auto done = token(timeline, 3), pending = token(timeline, 4);
	const void *handle = nullptr;
	ASSERT_EQ(refs.retain(&pending, 60, &handle), 0);
	ASSERT_NE(handle, nullptr);
	switchTimelineAtomicContext = true;
	EXPECT_EQ(refs.status(handle, &done), 1);
	EXPECT_EQ(refs.status(handle, &pending), 0);
	switchTimelineAtomicContext = false;
	switch_gpu_event_waits authority{1, 0};
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&pending, &authority, &node), 0);
	__atomic_store_n(&fence, 4, __ATOMIC_RELEASE);
	EXPECT_EQ(refs.status(handle, &pending), operations.status(&pending));
	EXPECT_EQ(refs.status(handle, &pending), 0);
	switch_gpu_timeline_semantic_finish(&node, -5);
	EXPECT_EQ(refs.status(handle, &pending), -5);
	EXPECT_EQ(refs.status(handle, &done), operations.status(&done));
	switch_gpu_timeline_close(timeline);
	refs.release(&pending, 59);
	EXPECT_EQ(fenceRefs.load(), 1u);
	EXPECT_EQ(refs.status(handle, &done), 1);
	refs.release(&done, 1);
	EXPECT_EQ(fenceRefs.load(), 0u);
	EXPECT_EQ(operations.status(&done), -19);
}

TEST_F(Timeline, FirstFailureIncludingZeroCannotBecomeSyntheticSuccess)
{
	volatile uint32_t fence = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	auto pending = token(timeline, 1);
	switch_gpu_timeline_failed(timeline, 0, 0);
	switch_gpu_timeline_failed(timeline, 100, 0);
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
			switch_gpu_timeline_failed(timeline, 1, 0);
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
	switch_gpu_timeline_failed(timeline, 1, 0);
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
	switch_gpu_timeline_failed(timeline, 1, 0);
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

TEST_F(Timeline, WaitLoopsWhenSemanticWaitFinishesBetweenStatusAndSnapshot)
{
	volatile uint32_t fence = 5;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	const auto current = token(timeline, 5);
	switch_gpu_event_waits authority;
	authority.onCachedStatus = [](switch_gpu_event_waits *batch) {
		if (__atomic_load_n(&batch->result, __ATOMIC_ACQUIRE) == 0) {
			__atomic_store_n(&batch->result, 1, __ATOMIC_RELEASE);
			return 0;
		}
		return 1;
	};
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&current, &authority, &node), 0);
	EXPECT_EQ(operations.wait(&current), 1);
	EXPECT_EQ(authority.waitCalls, 0u);
	switch_gpu_timeline_semantic_finish(&node, 0);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, WaitLoopsWhenSemanticNodeUnlinksConcurrently)
{
	for (unsigned round = 0; round < 500; ++round) {
		volatile uint32_t fence = 5;
		auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
		const auto current = token(timeline, 5);
		switch_gpu_event_waits authority;
		authority.onWait = [](switch_gpu_event_waits *batch) {
			int r = 0;
			while ((r = __atomic_load_n(&batch->result, __ATOMIC_ACQUIRE)) == 0)
				std::this_thread::yield();
			return r;
		};
		switch_gpu_timeline_semantic *node = nullptr;
		ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&current, &authority, &node), 0);
		std::atomic<bool> start{false};
		std::thread finisher([&] {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			__atomic_store_n(&authority.result, 1, __ATOMIC_RELEASE);
			switch_gpu_timeline_semantic_finish(&node, 0);
		});
		start.store(true, std::memory_order_release);
		EXPECT_EQ(operations.wait(&current), 1);
		finisher.join();
		switch_gpu_timeline_close(timeline);
	}
}

TEST_F(Timeline, WaitDrainsMultiplePendingSemanticNodesBeforePromoting)
{
	volatile uint32_t fence = 6;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	const auto fifth = token(timeline, 5), sixth = token(timeline, 6);
	switch_gpu_event_waits first, second;
	auto succeedOnWait = [](switch_gpu_event_waits *batch) {
		__atomic_store_n(&batch->result, 1, __ATOMIC_RELEASE);
		return 1;
	};
	first.onWait = succeedOnWait;
	second.onWait = succeedOnWait;
	switch_gpu_timeline_semantic *a = nullptr, *b = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&fifth, &first, &a), 0);
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&sixth, &second, &b), 0);
	EXPECT_EQ(operations.wait(&sixth), 1);
	EXPECT_EQ(first.waitCalls, 1u);
	EXPECT_EQ(second.waitCalls, 1u);
	EXPECT_EQ(switch_gpu_timeline_promote(&sixth), 1);
	switch_gpu_timeline_semantic_finish(&a, 0);
	switch_gpu_timeline_semantic_finish(&b, 0);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, WaitOnLaterSeqnoSucceedsWhenEarlierSemanticWaitFails)
{
	volatile uint32_t fence = 6;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	const auto fifth = token(timeline, 5), sixth = token(timeline, 6);
	switch_gpu_event_waits earlier, later;
	earlier.onWait = [](switch_gpu_event_waits *batch) {
		__atomic_store_n(&batch->result, -5, __ATOMIC_RELEASE);
		return -5;
	};
	later.onWait = [](switch_gpu_event_waits *batch) {
		__atomic_store_n(&batch->result, 1, __ATOMIC_RELEASE);
		return 1;
	};
	switch_gpu_timeline_semantic *a = nullptr, *b = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&fifth, &earlier, &a), 0);
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&sixth, &later, &b), 0);
	EXPECT_EQ(operations.wait(&sixth), 1);
	EXPECT_EQ(earlier.waitCalls, 1u);
	EXPECT_EQ(later.waitCalls, 1u);
	EXPECT_EQ(switch_gpu_timeline_promote(&sixth), 1);
	EXPECT_EQ(operations.status(&fifth), -5);
	switch_gpu_timeline_semantic_finish(&a, -5);
	switch_gpu_timeline_semantic_finish(&b, 0);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, WaitFailsWhenOwnSeqnoSemanticWaitFails)
{
	volatile uint32_t fence = 5;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	const auto fifth = token(timeline, 5);
	switch_gpu_event_waits own;
	own.onWait = [](switch_gpu_event_waits *batch) {
		__atomic_store_n(&batch->result, -5, __ATOMIC_RELEASE);
		return -5;
	};
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&fifth, &own, &node), 0);
	EXPECT_EQ(operations.wait(&fifth), -5);
	EXPECT_EQ(own.waitCalls, 1u);
	EXPECT_EQ(switch_gpu_timeline_promote(&fifth), -5);
	switch_gpu_timeline_semantic_finish(&node, -5);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, AbortedSemanticWaitDoesNotFailBeforeUnlink)
{
	volatile uint32_t fence = 5;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	const auto fifth = token(timeline, 5);
	switch_gpu_event_waits aborted;
	aborted.result = -125;
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&fifth, &aborted, &node), 0);
	EXPECT_EQ(operations.status(&fifth), 1);
	EXPECT_EQ(operations.wait(&fifth), 1);
	switch_gpu_event_waits *captured = nullptr;
	EXPECT_EQ(switch_gpu_timeline_semantic_snapshot(&fifth, &captured), 0);
	EXPECT_EQ(captured, nullptr);
	switch_gpu_timeline_semantic_finish(&node, -125);
	switch_gpu_timeline_close(timeline);
}

TEST_F(Timeline, ConcurrentSemanticFailureFinishNeverExposesFalseSuccess)
{
	for (unsigned round = 0; round < 500; ++round) {
		volatile uint32_t fence = 5;
		auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
		const auto fifth = token(timeline, 5);
		const void *handle = nullptr;
		ASSERT_EQ(refs.retain(&fifth, 1, &handle), 0);
		switch_gpu_event_waits authority;
		switch_gpu_timeline_semantic *node = nullptr;
		ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&fifth, &authority, &node), 0);
		std::atomic<bool> start{false}, finished{false};
		std::thread finisher([&] {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			switch_gpu_timeline_semantic_finish(&node, -5);
			finished.store(true, std::memory_order_release);
		});
		start.store(true, std::memory_order_release);
		do {
			EXPECT_NE(refs.status(handle, &fifth), 1);
			EXPECT_TRUE(switch_gpu_timeline_has_semantics());
		} while (!finished.load(std::memory_order_acquire));
		finisher.join();
		EXPECT_EQ(refs.status(handle, &fifth), -5);
		switch_gpu_timeline_close(timeline);
		refs.release(&fifth, 1);
	}
}

TEST_F(Timeline, ConcurrentFailureAndVisibilityUpdateNeverRace)
{
	for (unsigned round = 0; round < 500; ++round) {
		volatile uint32_t fence = 4, visible = 4;
		auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
		const auto pending = token(timeline, 5);
		std::atomic<bool> start{false};
		std::thread updater([&] {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			switch_gpu_timeline_visibility(timeline, &visible);
		});
		start.store(true, std::memory_order_release);
		switchTimelineAtomicContext = true;
		switch_gpu_timeline_failed(timeline, 4, 0);
		switchTimelineAtomicContext = false;
		updater.join();
		EXPECT_EQ(operations.status(&pending), -5);
		switch_gpu_timeline_close(timeline);
	}
}

// A channel's own host1x syncpoint (SwitchGpuTimelineSyncpt.cpp): a token is answered only inside its own drained job,
// with that job's threshold and last seqno; a token whose job did not drain would wait on a later job, which can wait on
// work after the consumer. The covering job's state comes from the fence alone, and a syncpoint goes back with the
// value every increment its channel pushed promised, for the bridge to make the ones that never ran.

TEST_F(Timeline, ATokenIsAnsweredOnlyInsideItsOwnDrainedJob)
{
	uint32_t ids[SWITCH_GPU_TIMELINE_SYNCPTS] = {};
	uint32_t promised[SWITCH_GPU_TIMELINE_SYNCPTS] = {};
	switch_gpu_timeline_syncpt_reclaim(ids, promised, SWITCH_GPU_TIMELINE_SYNCPTS);
	volatile uint32_t fence = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	ASSERT_NE(timeline, nullptr);
	const auto third = token(timeline, 3);
	const auto fifth = token(timeline, 5);
	const auto sixth = token(timeline, 6);
	uint32_t id = 0;
	uint32_t threshold = 0;
	uint32_t last = 0;
	EXPECT_EQ(switch_gpu_timeline_syncpoint(&third, &id, &threshold, &last), -61);
	EXPECT_EQ(switch_gpu_timeline_has_syncpt(timeline), 0);
	EXPECT_EQ(switch_gpu_timeline_syncpt_next(timeline, 3, 4), 0u);
	ASSERT_EQ(switch_gpu_timeline_syncpt_adopt(&third, 170, 40), 0);
	EXPECT_EQ(switch_gpu_timeline_has_syncpt(timeline), 1);
	EXPECT_EQ(switch_gpu_timeline_syncpt_adopt(&third, 171, 0), -16);

	EXPECT_EQ(switch_gpu_timeline_syncpt_next(timeline, 3, 4), 170u);
	EXPECT_EQ(switch_gpu_timeline_syncpt_next(timeline, 6, 6), 170u);
	ASSERT_EQ(switch_gpu_timeline_syncpoint(&third, &id, &threshold, &last), 1);
	EXPECT_EQ(id, 170u);
	EXPECT_EQ(threshold, 42u);
	EXPECT_EQ(last, 4u);
	EXPECT_EQ(switch_gpu_timeline_syncpoint(&fifth, &id, &threshold, &last), 0)
		<< "job 5 did not drain: job 6 would cover it, and job 6 may wait on the consumer";
	ASSERT_EQ(switch_gpu_timeline_syncpoint(&sixth, &id, &threshold, &last), 1);
	EXPECT_EQ(threshold, 44u);

	int stopped = -1;
	EXPECT_EQ(switch_gpu_timeline_covered(&third, 4, &stopped), 0);
	EXPECT_EQ(stopped, 0);
	__atomic_store_n(&fence, 4u, __ATOMIC_RELEASE);
	EXPECT_EQ(switch_gpu_timeline_covered(&third, 4, &stopped), 1);
	switch_gpu_timeline_failed(timeline, 4, 0);
	EXPECT_EQ(switch_gpu_timeline_covered(&sixth, 6, &stopped), -5);
	EXPECT_EQ(stopped, 1);
	EXPECT_EQ(switch_gpu_timeline_syncpoint(&sixth, &id, &threshold, &last), 0);

	switch_gpu_timeline_close(timeline);
	ASSERT_EQ(switch_gpu_timeline_syncpt_reclaim(ids, promised, SWITCH_GPU_TIMELINE_SYNCPTS),
		  1u);
	EXPECT_EQ(ids[0], 170u);
	EXPECT_EQ(promised[0], 44u) << "every increment the channel pushed";
}

// The syncpoint goes back only with the timeline's last reference: a consumer holding a token may still have a stream
// waiting on it, and by then the channel is torn down, so none of its increments can land on the next owner.
TEST_F(Timeline, ASyncpointGoesBackOnlyWithItsTimelinesLastReference)
{
	uint32_t ids[SWITCH_GPU_TIMELINE_SYNCPTS] = {};
	uint32_t promised[SWITCH_GPU_TIMELINE_SYNCPTS] = {};
	switch_gpu_timeline_syncpt_reclaim(ids, promised, SWITCH_GPU_TIMELINE_SYNCPTS);
	volatile uint32_t fence = 0;
	auto *timeline = switch_gpu_timeline_create(++nextId, &fence);
	ASSERT_NE(timeline, nullptr);
	const auto held = token(timeline, 1);
	ASSERT_EQ(switch_gpu_timeline_syncpt_adopt(&held, 172, 0), 0);
	ASSERT_EQ(operations.retain_token(&held), 0);
	switch_gpu_timeline_close(timeline);
	EXPECT_EQ(switch_gpu_timeline_syncpt_reclaim(ids, promised, SWITCH_GPU_TIMELINE_SYNCPTS),
		  0u);
	operations.release_token(&held);
	ASSERT_EQ(switch_gpu_timeline_syncpt_reclaim(ids, promised, SWITCH_GPU_TIMELINE_SYNCPTS),
		  1u);
	EXPECT_EQ(ids[0], 172u);
}
