// SPDX-License-Identifier: MIT
#include "drivers/TegraGM20B/SwitchGpuFenceWaiter.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// A job of seqnos 41..43, 41 and 42 its internal fences: a waiter for the job's end sleeps through both.
TEST(FenceWaiter, InternalFencesDoNotWakeAWaiterForTheJobsEnd)
{
	uint64_t wanted = 0;
	switch_gpu_waiter_want(&wanted, 43);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 41), 0);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 42), 0);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 43), 1);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 43), 0);
}

TEST(FenceWaiter, TheLowestSeqnoWakesAndALaterWaiterPublishesAgain)
{
	uint64_t wanted = 0;
	switch_gpu_waiter_want(&wanted, 60);
	switch_gpu_waiter_want(&wanted, 50);
	switch_gpu_waiter_want(&wanted, 70);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 49), 0);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 55), 1);
	switch_gpu_waiter_want(&wanted, 70);
	switch_gpu_waiter_want(&wanted, 60);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 59), 0);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 60), 1);
}

TEST(FenceWaiter, NoWaiterNoWake)
{
	uint64_t wanted = 0;
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 0), 0);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 1000), 0);
}

TEST(FenceWaiter, WrappedSequencesCompareInOrder)
{
	uint64_t wanted = 0;
	switch_gpu_waiter_want(&wanted, 2);
	switch_gpu_waiter_want(&wanted, UINT32_MAX);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, UINT32_MAX - 1), 0);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, UINT32_MAX), 1);
	switch_gpu_waiter_want(&wanted, 2);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 1), 0);
	EXPECT_EQ(switch_gpu_waiter_reached(&wanted, 2), 1);
}

// The kernel's protocol on real threads: a waiter takes the word's sequence, publishes, tests the fence and sleeps only
// while the word is unchanged; the GPU moves the fence one seqno at a time and the pass bumps the word only when a
// published seqno is reached. A lost wake leaves a waiter asleep past its 5 s deadline.
TEST(FenceWaiter, EveryWaiterWakesAndFewerWakesThanFences)
{
	constexpr uint32_t kFences = 20000;
	constexpr int kWaiters = 4;
	std::atomic<uint32_t> fence{0}, word{0};
	std::atomic<bool> lost{false};
	std::atomic<int> left{kWaiters};
	uint64_t wanted = 0;
	std::mutex lock;
	std::condition_variable woken;
	uint32_t wakes = 0;

	std::vector<std::thread> waiters;
	for (int w = 0; w < kWaiters; ++w) {
		waiters.emplace_back([&, w] {
			for (uint32_t target = 7 + w; target <= kFences; target += 97 + 13 * w) {
				for (;;) {
					const uint32_t seq = word.load(std::memory_order_acquire);
					switch_gpu_waiter_want(&wanted, target);
					const uint32_t done = fence.load(std::memory_order_relaxed);
					if (int32_t(done - target) >= 0)
						break;
					const auto moved = [&] {
						return word.load(std::memory_order_acquire) != seq;
					};
					std::unique_lock held(lock);
					if (!woken.wait_for(held, std::chrono::seconds(5), moved)) {
						lost = true;
						return;
					}
				}
			}
			--left;
		});
	}
	for (uint32_t done = 1; done <= kFences + 1000 && left.load() > 0; ++done) {
		fence.store(done, std::memory_order_relaxed);
		if (switch_gpu_waiter_reached(&wanted, done)) {
			std::lock_guard held(lock);
			++wakes;
			word.fetch_add(1, std::memory_order_release);
			woken.notify_all();
		}
		if (done % 64 == 0)
			std::this_thread::yield();
	}
	for (auto &t : waiters)
		t.join();
	EXPECT_FALSE(lost.load());
	EXPECT_EQ(left.load(), 0);
	EXPECT_LT(wakes, kFences / 4);
}
