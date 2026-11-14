// SPDX-License-Identifier: MIT
// SwitchGpuChanTable.h: channels created and torn down while the interrupt pass completes them. The pass runs as a
// work loop action and a teardown clears the channel's slot, then runs an empty action behind it before freeing the
// channel: no pass ever touches a freed channel (ASan and ThreadSanitizer runs).
#include <gtest/gtest.h>
#include <atomic>
#include <mutex>
#include <thread>
#include "drivers/TegraGM20B/SwitchGpuChanTable.h"

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
