// SPDX-License-Identifier: MIT
// SwitchGpuWakeWord.h: an interrupt pass with nobody asleep on the global word wakes no thread and writes only its own
// sequence; a sleeper racing a pass, and another waker beside it, never misses its wake (a lost one would hang the
// waiter here).
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "drivers/TegraGM20B/SwitchGpuWakeWord.h"

namespace
{
// xnu_glue.cpp's wakeAfterPass, nvxnu_wake and nvxnu_idle over a mutex and condition variable standing in for
// assert_wait and thread_wakeup on the word's address
struct Word {
	switch_gpu_wake_word word = {};
	std::mutex lock;
	std::condition_variable asleep;
	std::atomic<unsigned> wakeups{0};

	void wakeup()
	{
		std::lock_guard<std::mutex> held(lock);
		++wakeups;
		asleep.notify_all();
	}

	void pass()
	{
		if (switch_gpu_wake_pass(&word))
			wakeup();
	}

	void other()
	{
		if (switch_gpu_wake_other(&word))
			wakeup();
	}

	void idle(uint32_t seq)
	{
		switch_gpu_wake_sleep(&word, 1);
		{
			std::unique_lock<std::mutex> held(lock);
			asleep.wait(held, [&] {
				return switch_gpu_wake_seq(&word) != seq;
			});
		}
		switch_gpu_wake_sleep(&word, 0);
	}
};
}  // namespace

// A compositor frame's passes (two jobs, each its own interrupt) with no kernel thread asleep on the word.
TEST(WakeWord, PassesWithNobodyAsleepWakeNoThread)
{
	Word w;
	for (int pass = 0; pass < 2; ++pass)
		w.pass();
	EXPECT_EQ(w.wakeups.load(), 0u);
	EXPECT_EQ(w.word.pass, 2u);
	EXPECT_EQ(w.word.others, 0u) << "the pass writes only its own sequence";
	EXPECT_EQ(switch_gpu_wake_seq(&w.word), 2u);
	switch_gpu_wake_sleep(&w.word, 1);
	w.pass();
	EXPECT_EQ(w.wakeups.load(), 1u) << "a sleeper is woken";
	switch_gpu_wake_sleep(&w.word, 0);
}

TEST(WakeWord, ASleeperRacingThePassAndAnotherWakerIsWoken)
{
	constexpr unsigned kRounds = 20000;
	Word w;
	std::atomic<unsigned> ready{0}, done{0};
	std::atomic<bool> stop{false};
	std::thread waiter([&] {
		for (unsigned round = 1; round <= kRounds; ++round) {
			for (;;) {
				const uint32_t seq = switch_gpu_wake_seq(&w.word);
				if (ready.load(std::memory_order_acquire) >= round)
					break;
				w.idle(seq);
			}
			done.store(round, std::memory_order_release);
		}
	});
	std::thread others([&] {
		while (!stop.load(std::memory_order_acquire))
			w.other();
	});
	for (unsigned round = 1; round <= kRounds; ++round) {
		ready.store(round, std::memory_order_release);
		w.pass();
		while (done.load(std::memory_order_acquire) != round)
			;
	}
	stop.store(true, std::memory_order_release);
	others.join();
	waiter.join();
	EXPECT_EQ(done.load(), kRounds);
}
