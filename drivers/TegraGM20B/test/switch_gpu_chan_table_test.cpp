// SPDX-License-Identifier: MIT
// SwitchGpuChanTable.h: channels created and torn down while the interrupt pass completes them. The pass runs as a
// work loop action and a teardown clears the channel's slot, then runs an empty action behind it before freeing the
// channel: no pass ever touches a freed channel (ASan and ThreadSanitizer runs).
#include <gtest/gtest.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuChanTable.h"
#include "drivers/TegraGM20B/SwitchGpuLiveDir.h"

namespace
{
struct Chan {
	std::atomic<uint32_t> completions{0};
};

std::mutex workLoop;  // the work loop's gate: one action at a time

void pass(switch_gpu_chan_table &table, uint32_t slots)
{
	std::lock_guard<std::mutex> action(workLoop);
	for (uint32_t chid = 0; chid < slots; ++chid) {
		auto *chan = static_cast<Chan *>(switch_gpu_chan_table_get(&table, chid));
		if (chan)
			chan->completions.fetch_add(1, std::memory_order_relaxed);
	}
}

void passBarrier()
{
	std::lock_guard<std::mutex> action(workLoop);
}
}  // namespace

TEST(ChanTable, TeardownRacingThePassNeverFreesAChannelInUse)
{
	constexpr uint32_t kSlots = 8, kRounds = 20000;
	switch_gpu_chan_table table = {};
	std::atomic<bool> stop{false};
	std::thread interrupts([&] {
		while (!stop.load(std::memory_order_acquire))
			pass(table, kSlots);
	});
	for (uint32_t round = 0; round < kRounds; ++round) {
		const uint32_t chid = round % kSlots;
		auto *chan = new Chan;
		switch_gpu_chan_table_set(&table, chid, chan);
		std::this_thread::yield();
		switch_gpu_chan_table_set(&table, chid, nullptr);
		passBarrier();
		delete chan;
	}
	stop.store(true, std::memory_order_release);
	interrupts.join();
	for (uint32_t chid = 0; chid < kSlots; ++chid)
		EXPECT_EQ(switch_gpu_chan_table_get(&table, chid), nullptr);
}

TEST(LiveDir, ConcurrentFindMatchesPublishedTupleAndHandlesHintCollisions)
{
	constexpr uint32_t kSlots = 16;
	constexpr uint32_t kRounds = 8000;
	switch_gpu_live_dir dir = {};
	std::mutex writerLock;
	std::atomic<bool> stop{false};

	// Seed two channels whose uids collide in the 1024-entry hint table.
	const uint64_t stableA = (UINT64_C(3) << 32) | 42u;
	const uint64_t stableB = (UINT64_C(5) << 32) | (42u + SWITCH_GPU_LIVE_HINTS);
	const int vmA = 1, vmB = 2;
	switch_gpu_live_dir_set(&dir, 0, stableA, 0x4000u, &vmA);
	switch_gpu_live_dir_set(&dir, 1, stableB, 0x8000u, &vmB);

	std::vector<std::thread> readers;
	for (int r = 0; r < 4; ++r) {
		readers.emplace_back([&, r] {
			uint32_t iter = 0;
			while (!stop.load(std::memory_order_acquire)) {
				switch_gpu_live_info info = {};
				ASSERT_EQ(switch_gpu_live_dir_find(&dir, 42u, &info), 1);
				EXPECT_EQ(info.id, stableA);
				EXPECT_EQ(info.fence_offset, 0x4000u);
				EXPECT_EQ(info.chid, 0u);
				EXPECT_EQ(info.vm, &vmA);

				ASSERT_EQ(switch_gpu_live_dir_find(
						  &dir, 42u + SWITCH_GPU_LIVE_HINTS, &info),
					  1);
				EXPECT_EQ(info.id, stableB);
				EXPECT_EQ(info.fence_offset, 0x8000u);
				EXPECT_EQ(info.chid, 1u);
				EXPECT_EQ(info.vm, &vmB);

				const uint32_t uid = 100u + ((iter + uint32_t(r)) % (kSlots - 2u));
				if (switch_gpu_live_dir_find(&dir, uid, &info)) {
					EXPECT_EQ(uint32_t(info.id), uid);
					EXPECT_EQ(info.fence_offset, info.chid * 0x4000u);
					EXPECT_EQ(info.vm, reinterpret_cast<const void *>(uintptr_t(
								   info.id & 0xffffffffu)));
				}
				++iter;
			}
		});
	}

	for (uint32_t round = 0; round < kRounds; ++round) {
		const uint32_t slot = 2u + (round % (kSlots - 2u));
		const uint32_t uid = 100u + (slot - 2u);
		const uint64_t gen = uint64_t(round / (kSlots - 2u) + 1u);
		const uint64_t id = (gen << 32) | uid;
		std::lock_guard<std::mutex> guard(writerLock);
		switch_gpu_live_dir_set(&dir, slot, id, slot * 0x4000u,
					reinterpret_cast<const void *>(uintptr_t(uid)));
		if (round & 1u)
			switch_gpu_live_dir_clear(&dir, slot);
	}

	stop.store(true, std::memory_order_release);
	for (auto &t : readers)
		t.join();

	for (uint32_t slot = 0; slot < kSlots; ++slot)
		switch_gpu_live_dir_clear(&dir, slot);
	switch_gpu_live_info info = {};
	EXPECT_EQ(switch_gpu_live_dir_find(&dir, 42u, &info), 0);
	EXPECT_EQ(switch_gpu_live_dir_find(&dir, 42u + SWITCH_GPU_LIVE_HINTS, &info), 0);
}
