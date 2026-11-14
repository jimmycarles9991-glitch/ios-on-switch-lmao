// SPDX-License-Identifier: MIT
// SwitchGpuBusy.h: each channel's kicked and done words, each with one writer, read together by a high kick on another
// thread. A reader never finds a channel idle once its kick is published and before its done word reaches that seqno,
// whatever the interleaving; and a done word left behind reads as work in flight.
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "drivers/TegraGM20B/SwitchGpuBusy.h"

TEST(GpuBusy, AReaderNeverFindsPublishedWorkIdle)
{
	constexpr uint32_t kChid = 77, kRounds = 100000;
	switch_gpu_busy busy = {};
	std::atomic<uint32_t> kicked{0}, done{0};
	std::atomic<bool> stop{false};
	std::atomic<unsigned> idleWithWork{0}, reads{0};
	switch_gpu_busy_level(&busy, kChid, 1);
	// the pass: each seqno the owner kicked, published done (the test's own record of it first)
	std::thread pass([&] {
		uint32_t seen = 0;
		while (!stop.load(std::memory_order_acquire)) {
			const uint32_t newest = kicked.load(std::memory_order_acquire);
			if (newest != seen) {
				seen = newest;
				done.store(seen, std::memory_order_release);
				switch_gpu_busy_done(&busy, kChid, seen);
			}
		}
	});
	// a high kick: idle is wrong while a kick published before the look has not been seen done after it
	std::thread reader([&] {
		while (!stop.load(std::memory_order_acquire)) {
			const uint32_t before = kicked.load(std::memory_order_acquire);
			const int busyNow = switch_gpu_busy_low(&busy);
			const uint32_t after = done.load(std::memory_order_acquire);
			if (busyNow == 0 && before > after)
				idleWithWork.fetch_add(1);
			reads.fetch_add(1);
		}
	});
	for (uint32_t round = 1; round <= kRounds; ++round) {
		switch_gpu_busy_kick(&busy, kChid, round);
		kicked.store(round, std::memory_order_release);
		while (done.load(std::memory_order_acquire) != round)
			;
	}
	stop.store(true, std::memory_order_release);
	pass.join();
	reader.join();
	EXPECT_EQ(idleWithWork.load(), 0u);
	EXPECT_GT(reads.load(), 0u);
	EXPECT_FALSE(switch_gpu_busy_low(&busy));
}

TEST(GpuBusy, OnlyLiveLowChannelsCount)
{
	switch_gpu_busy busy = {};
	switch_gpu_busy_open(&busy, 3, 10);
	switch_gpu_busy_kick(&busy, 3, 11);
	EXPECT_FALSE(switch_gpu_busy_low(&busy)) << "a channel not listed below the high level";
	uint64_t slow[SWITCH_GPU_BUSY_WORDS] = {};
	slow[200 / 64] = UINT64_C(1) << (200 % 64);
	switch_gpu_busy_open(&busy, 200, 7);
	switch_gpu_busy_level(&busy, 200, 1);
	EXPECT_FALSE(switch_gpu_busy_among(&busy, slow));
	switch_gpu_busy_kick(&busy, 200, 8);
	EXPECT_TRUE(switch_gpu_busy_low(&busy));
	EXPECT_TRUE(switch_gpu_busy_among(&busy, slow));
	switch_gpu_busy_done(&busy, 200, 7);
	EXPECT_TRUE(switch_gpu_busy_chan(&busy, 200)) << "a done word behind the newest kick";
	switch_gpu_busy_level(&busy, 200, 0);
	EXPECT_FALSE(switch_gpu_busy_low(&busy)) << "torn down";
	EXPECT_EQ(switch_gpu_busy_chan(&busy, 512), 0) << "no such channel";
}
