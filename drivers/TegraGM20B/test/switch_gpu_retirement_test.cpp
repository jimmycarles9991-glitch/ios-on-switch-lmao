// SPDX-License-Identifier: MIT
#include <atomic>
#include "bazel/testing/require.h"
#include <gtest/gtest.h>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
using namespace std::chrono_literals;
extern "C" void switch_gpu_backing_retire(void);
extern "C" void switch_gpu_backing_expire(void);

namespace
{
struct State {
	uint64_t completed = 0;
	unsigned refs = 0;
	int error = 0;
};

std::mutex timelineMutex;
std::condition_variable woke;
std::map<unsigned, State> timelines;
std::atomic<unsigned> scheduled{0}, waits{0}, releases{0}, statusCalls{0}, arms{0};
std::atomic<uint64_t> now{100}, armed{0};
std::atomic<bool> waitEntered{false}, unblockWait{true}, completeInStatus{false};
uint64_t page = 0x80000000;
// A new backing counts as CPU-written, so its first GM20B job keeps a second reference as the backing's host acquire.
constexpr unsigned kPublished = 2;

int status(const switch_gpu_backing_token *token)
{
	++statusCalls;
	std::lock_guard<std::mutex> lock(timelineMutex);
	auto &state = timelines[token->queue];
	const int result = state.completed >= token->value ? 1 : state.error;
	if (completeInStatus.exchange(false)) {
		state.completed = token->value;
		switch_gpu_backing_completed();
	}
	return result;
}

int wait(const switch_gpu_backing_token *token)
{
	++waits;
	waitEntered = true;
	std::unique_lock<std::mutex> lock(timelineMutex);
	woke.wait(lock, [] {
		return unblockWait.load();
	});
	auto &state = timelines[token->queue];
	state.completed = token->value;
	return 1;
}

int retain(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> lock(timelineMutex);
	++timelines[token->queue].refs;
	return 0;
}

void drop(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> lock(timelineMutex);
	REQUIRE(timelines[token->queue].refs);
	--timelines[token->queue].refs;
}

void ownerRelease(void *)
{
	++releases;
}

switch_gpu_backing *make()
{
	page += 4096;
	auto *backing = switch_gpu_backing_register(&page, 1, 0, &releases, ownerRelease);
	REQUIRE(backing);
	return backing;
}

void publish(switch_gpu_backing *backing, unsigned queue)
{
	switch_gpu_backing_token completion{1, 1, SWITCH_GPU_ENGINE_GM20B, queue};
	switch_gpu_backing_use use{backing, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction job{};
	REQUIRE(!switch_gpu_backing_begin(&use, 1, &completion, &job));
	switch_gpu_backing_commit(&job);
}

void complete(unsigned queue, int error = 0)
{
	{
		std::lock_guard<std::mutex> lock(timelineMutex);
		if (error)
			timelines[queue].error = error;
		else
			timelines[queue].completed = 1;
	}
	switch_gpu_backing_completed();
}

void reset()
{
	switch_gpu_backing_retire();
	for (const auto &entry : timelines)
		REQUIRE(entry.second.refs == 0);
	timelines.clear();
	scheduled = waits = releases = statusCalls = arms = 0;
	armed = 0;
	now = 100;
}
}

extern "C" int switch_gpu_backing_prepare_retirement()
{
	return 0;
}

extern "C" void switch_gpu_backing_schedule_retirement()
{
	++scheduled;
}

extern "C" uint64_t switch_gpu_backing_time_ns()
{
	return now.load();
}

extern "C" void switch_gpu_backing_arm_expiry(uint64_t deadline)
{
	++arms;
	armed = deadline;
}

extern "C" {
struct tgm_coh;

tgm_coh *tgm_coh_find_pa(uint64_t)
{
	return nullptr;
}

void tgm_views_sweep(tgm_coh *)
{
}

void tgm_views_deferred_lock(uint32_t, uint32_t)
{
}

void tgm_coherency_put(tgm_coh *)
{
}

int tegra_gm20b_surface_tiled(uint64_t)
{
	return 0;
}

int switch_gpu_surface_prepare(uint32_t, void *, uint32_t, switch_gpu_backing **out,
			       switch_gpu_cpu_lease **owned, void **descriptor)
{
	*out = nullptr;
	*owned = nullptr;
	*descriptor = nullptr;
	return 0;
}

void switch_gpu_surface_finish(void *)
{
}

uint32_t switch_gpu_surface_users(void *)
{
	return 0;
}

void switch_gpu_surface_descriptor_hold(void *, int)
{
}
}

extern "C" void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t, uint64_t, int)
{
}

class Retirement : public testing::Test
{
protected:
	void SetUp() override
	{
		static bool registered;
		if (!registered) {
			const switch_gpu_backing_engine engine{status, wait, retain, drop, 64};
			ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_GM20B,
								     &engine),
				  0);
			registered = true;
		}
		reset();
	}

	void TearDown() override
	{
		reset();
	}
};

TEST_F(Retirement, PendingHeadDoesNotBlockReadyTail)
{
	auto *ready = make(), *pending = make();
	publish(ready, 1);
	publish(pending, 2);
	complete(1);
	switch_gpu_backing_release(pending);
	switch_gpu_backing_release(ready);
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 1 && waits == 0 && timelines[2].refs == kPublished);
	complete(2);
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 2 && armed == 0);
}

TEST_F(Retirement, CompletionDuringScanSchedulesAnotherPass)
{
	auto *backing = make();
	publish(backing, 1);
	switch_gpu_backing_release(backing);
	scheduled = 0;
	completeInStatus = true;
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 0 && scheduled >= 1 && waits == 0);
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 1);
}

TEST_F(Retirement, RevivedAllocationKeepsOwner)
{
	auto *backing = make();
	const uint64_t first = page;
	publish(backing, 1);
	switch_gpu_backing_release(backing);
	auto *revived = switch_gpu_backing_find_pa(first);
	ASSERT_TRUE(revived == backing);
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 0 && armed == 0);
	complete(1);
	switch_gpu_backing_release(revived);
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 1 && !switch_gpu_backing_find_pa(first));
}

TEST_F(Retirement, RecoveryStartsOnlyAtDeadlineAndRetainsToken)
{
	auto *backing = make();
	publish(backing, 1);
	switch_gpu_backing_release(backing);
	ASSERT_TRUE(armed == 10000000100ULL);
	switch_gpu_backing_expire();
	ASSERT_TRUE(waits == 0 && releases == 0);
	now = armed.load();
	switch_gpu_backing_expire();
	ASSERT_TRUE(waits == 1 && releases == 0 && timelines[1].refs == kPublished);
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 1);
}

TEST_F(Retirement, BlockedRecoveryDoesNotStopReadyReclamation)
{
	auto *stuck = make();
	publish(stuck, 1);
	switch_gpu_backing_release(stuck);
	unblockWait = false;
	waitEntered = false;
	now = armed.load();
	auto recovery = std::async(std::launch::async, [] {
		switch_gpu_backing_expire();
	});
	for (unsigned i = 0; !waitEntered && i < 100000; ++i)
		std::this_thread::yield();
	ASSERT_TRUE(waitEntered && timelines[1].refs == kPublished + 1);
	auto *ready = make();
	publish(ready, 2);
	complete(2);
	switch_gpu_backing_release(ready);
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 1 && waits == 1);
	{
		std::lock_guard<std::mutex> lock(timelineMutex);
		unblockWait = true;
	}
	woke.notify_all();
	ASSERT_TRUE(recovery.wait_for(2s) == std::future_status::ready);
	recovery.get();
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 2);
}

TEST_F(Retirement, ConfirmedFailureRetiresAndIdleNotificationsDoNothing)
{
	auto *backing = make();
	publish(backing, 1);
	switch_gpu_backing_release(backing);
	complete(1, -5);
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 1 && waits == 0);
	scheduled = 0;
	switch_gpu_backing_completed();
	ASSERT_TRUE(!scheduled);
}

TEST_F(Retirement, ReleaseBurstArmsOnceAndScansBoundedBatches)
{
	std::vector<switch_gpu_backing *> all;
	for (unsigned i = 1; i <= 200; ++i) {
		auto *backing = make();
		publish(backing, i);
		if (i > 129)
			complete(i);
		all.push_back(backing);
	}
	for (auto *backing : all)
		switch_gpu_backing_release(backing);
	ASSERT_TRUE(arms == 1);
	for (unsigned i = 0; i < 4; ++i) {
		statusCalls = 0;
		switch_gpu_backing_retire();
		ASSERT_TRUE(statusCalls <= 64);
	}
	ASSERT_TRUE(releases == 71 && waits == 0);
	for (unsigned i = 1; i <= 129; ++i)
		complete(i);
	for (unsigned i = 0; i < 3; ++i)
		switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 200 && armed == 0);
}

TEST_F(Retirement, AppendDuringContinuationCannotLoseWake)
{
	for (unsigned i = 1; i <= 130; ++i) {
		auto *backing = make();
		publish(backing, i);
		switch_gpu_backing_release(backing);
	}
	switch_gpu_backing_retire();
	auto *added = make();
	publish(added, 131);
	complete(131);
	switch_gpu_backing_release(added);
	switch_gpu_backing_retire();
	scheduled = 0;
	switch_gpu_backing_retire();
	ASSERT_TRUE(scheduled > 0 && releases == 0);
	for (unsigned i = 0; i < 3; ++i)
		switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 1);
	for (unsigned i = 1; i <= 130; ++i)
		complete(i);
	for (unsigned i = 0; i < 5; ++i)
		switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 131 && armed == 0);
}

TEST_F(Retirement, ConcurrentReleaseCompletionAndSweepKeepLifetimes)
{
	constexpr unsigned count = 400;
	std::vector<switch_gpu_backing *> all;
	for (unsigned i = 1; i <= count; ++i) {
		auto *backing = make();
		publish(backing, i);
		all.push_back(backing);
	}
	std::atomic<bool> freeingDone{false}, completionDone{false};
	std::thread freeing([&] {
		for (auto *backing : all) {
			switch_gpu_backing_release(backing);
			std::this_thread::yield();
		}
		freeingDone = true;
	});
	std::thread completing([&] {
		for (unsigned i = count; i; --i) {
			complete(i);
			std::this_thread::yield();
		}
		completionDone = true;
	});
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (releases < count && std::chrono::steady_clock::now() < deadline) {
		switch_gpu_backing_retire();
		std::this_thread::yield();
	}
	freeing.join();
	completing.join();
	ASSERT_TRUE(freeingDone && completionDone && releases == count && waits == 0);
}

TEST_F(Retirement, ConsumedTimerRearmsEqualDeadlineOnAnotherQueue)
{
	for (unsigned i = 1; i <= 3; ++i) {
		auto *backing = make();
		publish(backing, i);
		switch_gpu_backing_release(backing);
	}
	const auto due = armed.load();
	ASSERT_TRUE(arms == 1);
	now = due;
	for (unsigned i = 1; i <= 3; ++i) {
		armed = 0;
		const auto before = arms.load();
		switch_gpu_backing_expire();
		ASSERT_TRUE(waits == i && arms == before + 1);
		ASSERT_TRUE(armed == (i < 3 ? due : due + 10000000000ULL));
	}
	switch_gpu_backing_retire();
	ASSERT_TRUE(releases == 3 && armed == 0);
}
