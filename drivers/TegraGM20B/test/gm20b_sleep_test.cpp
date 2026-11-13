// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/TegraGM20BSleepGate.h on the host: the gate every GPU-touching path outside tgm_gpu.lock goes through for
// system sleep, single-threaded (each transition) and on threads with the kernel's protocol around it (gpu_bringup.c tgm_gate_enter,
// gate_try, gate_leave, sleep_gate, wake_open) on a std::mutex and condition variables in place of lck_mtx_sleep and
// thread_wakeup; and the sleep plan for each bring-up stage.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../TegraGM20BSleepGate.h"

namespace
{
TEST(GM20BSleepGate, OpenLetsCallersInAndOut)
{
	tgm_gate g = {};
	EXPECT_EQ(tgm_gate_try_enter(&g), TGM_GATE_IN);
	EXPECT_EQ(tgm_gate_try_enter(&g), TGM_GATE_IN);
	EXPECT_EQ(g.inside, 2u);
	EXPECT_FALSE(tgm_gate_leave(&g)) << "an open gate has no closer to wake";
	EXPECT_FALSE(tgm_gate_leave(&g));
	EXPECT_EQ(g.inside, 0u);
	EXPECT_FALSE(tgm_gate_drained(&g)) << "drained means shut and empty";
}

TEST(GM20BSleepGate, CloseWithNobodyInsideIsDrainedAtOnce)
{
	tgm_gate g = {};
	tgm_gate_close(&g);
	EXPECT_TRUE(tgm_gate_is_shut(&g));
	EXPECT_TRUE(tgm_gate_drained(&g));
	EXPECT_EQ(tgm_gate_try_enter(&g), TGM_GATE_SHUT);
	EXPECT_EQ(g.inside, 0u) << "a caller turned back is not counted";
}

TEST(GM20BSleepGate, LastLeaveWakesTheCloser)
{
	tgm_gate g = {};
	ASSERT_EQ(tgm_gate_try_enter(&g), TGM_GATE_IN);
	ASSERT_EQ(tgm_gate_try_enter(&g), TGM_GATE_IN);
	tgm_gate_close(&g);
	EXPECT_FALSE(tgm_gate_drained(&g));
	EXPECT_FALSE(tgm_gate_leave(&g)) << "one is still inside";
	EXPECT_FALSE(tgm_gate_drained(&g));
	EXPECT_TRUE(tgm_gate_leave(&g)) << "the last one out wakes the closer";
	EXPECT_TRUE(tgm_gate_drained(&g));
}

TEST(GM20BSleepGate, ABackedOutEnterThatDrainsTheGateWakesTheCloser)
{
	// The race try_enter covers: a caller that counted itself in, then saw the gate shut. Replayed by hand: the closer
	// shuts the gate between the caller's first check and its add.
	tgm_gate g = {};
	__atomic_add_fetch(&g.inside, 1, __ATOMIC_SEQ_CST);
	tgm_gate_close(&g);
	EXPECT_FALSE(tgm_gate_drained(&g)) << "the closer sees the caller and must wait for it";
	const bool wake = __atomic_sub_fetch(&g.inside, 1, __ATOMIC_SEQ_CST) == 0;
	EXPECT_TRUE(wake);
	EXPECT_TRUE(tgm_gate_drained(&g));
	// Through the function: a shut gate with one caller inside turns a second back without counting it.
	tgm_gate h = {};
	ASSERT_EQ(tgm_gate_try_enter(&h), TGM_GATE_IN);
	tgm_gate_close(&h);
	EXPECT_EQ(tgm_gate_try_enter(&h), TGM_GATE_SHUT);
	EXPECT_EQ(h.inside, 1u);
	EXPECT_TRUE(tgm_gate_leave(&h));
}

TEST(GM20BSleepGate, OpenAgainAfterTheWake)
{
	tgm_gate g = {};
	tgm_gate_close(&g);
	ASSERT_TRUE(tgm_gate_drained(&g));
	tgm_gate_open(&g);
	EXPECT_FALSE(tgm_gate_is_shut(&g));
	EXPECT_EQ(tgm_gate_try_enter(&g), TGM_GATE_IN);
	EXPECT_FALSE(tgm_gate_leave(&g));
	tgm_gate_close(&g);  // the next sleep
	EXPECT_TRUE(tgm_gate_drained(&g));
}

// gpu_bringup.c's protocol on host primitives: one mutex, `shutCv` for callers waiting for the open (event &gate.shut), `insideCv`
// for the closer (event &gate.inside). Every wake is under the mutex, as gate_wake_closer and wake_open take tgm_gate_lock.
struct Kernel {
	tgm_gate gate = {};
	std::mutex lock;
	std::condition_variable shutCv, insideCv;

	void wakeCloser()
	{
		std::lock_guard<std::mutex> hold(lock);
		insideCv.notify_all();
	}

	void enter()  // tgm_gate_enter
	{
		tgm_gate_entry entry;
		while ((entry = tgm_gate_try_enter(&gate)) != TGM_GATE_IN) {
			std::unique_lock<std::mutex> hold(lock);
			if (entry == TGM_GATE_SHUT_WAKE)
				insideCv.notify_all();
			shutCv.wait(hold, [&] {
				return !tgm_gate_is_shut(&gate);
			});
		}
	}

	bool tryEnter()  // gate_try
	{
		const tgm_gate_entry entry = tgm_gate_try_enter(&gate);
		if (entry == TGM_GATE_SHUT_WAKE)
			wakeCloser();
		return entry == TGM_GATE_IN;
	}

	void leave()  // gate_leave
	{
		if (tgm_gate_leave(&gate))
			wakeCloser();
	}

	// sleep_gate; false: still not drained at the bound (where the kernel panics)
	bool close(std::chrono::milliseconds bound)
	{
		std::unique_lock<std::mutex> hold(lock);
		tgm_gate_close(&gate);
		return insideCv.wait_for(hold, bound, [&] {
			return tgm_gate_drained(&gate) != 0;
		});
	}

	void open()  // wake_open
	{
		std::lock_guard<std::mutex> hold(lock);
		tgm_gate_open(&gate);
		shutCv.notify_all();
	}
};

TEST(GM20BSleepGate, NoCallerIsInsideBetweenTheDrainAndTheOpen)
{
	constexpr int kSubmitters = 6, kRefusable = 2, kSleeps = 200;
	Kernel k;
	std::atomic<int> inside{0}, violations{0};
	std::atomic<bool> stop{false}, powered{true};
	std::atomic<long> calls{0}, refused{0};
	auto work = [&] {
		if (!powered.load())
			violations++;
		inside++;
		std::this_thread::yield();
		inside--;
		calls++;
	};
	std::vector<std::thread> threads;
	for (int i = 0; i < kSubmitters; i++)  // tgm_chan_exec, tgm_chan_new: wait for the wake
		threads.emplace_back([&] {
			while (!stop.load()) {
				k.enter();
				work();
				k.leave();
			}
		});
	for (int i = 0; i < kRefusable; i++)  // tgm_present_copy, the scrubber: refused while shut
		threads.emplace_back([&] {
			while (!stop.load()) {
				if (!k.tryEnter()) {
					refused++;
					std::this_thread::yield();
					continue;
				}
				work();
				k.leave();
			}
		});
	for (int s = 0; s < kSleeps; s++) {
		ASSERT_TRUE(k.close(std::chrono::seconds(5)))
			<< "sleep " << s << ": the gate never drained";
		EXPECT_EQ(inside.load(), 0) << "sleep " << s << ": a caller inside a drained gate";
		powered = false;  // the GPU is off from here to the wake
		std::this_thread::sleep_for(std::chrono::microseconds(200));
		EXPECT_EQ(inside.load(), 0) << "sleep " << s << ": a caller got in while shut";
		powered = true;
		k.open();
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	stop = true;
	k.open();
	for (auto &t : threads)
		t.join();
	EXPECT_EQ(violations.load(), 0) << "GPU work while it was powered off";
	EXPECT_GT(calls.load(), kSleeps) << "callers kept going between sleeps";
	EXPECT_GT(refused.load(), 0) << "the refusable callers met the shut gate";
}

TEST(GM20BSleepGate, ASubmitterWaitingAtTheGateGoesOnAtTheWake)
{
	Kernel k;
	ASSERT_TRUE(k.close(std::chrono::seconds(1)));
	std::atomic<bool> done{false};
	std::thread submitter([&] {
		k.enter();
		done = true;
		k.leave();
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_FALSE(done.load()) << "a submission ran while the GPU slept";
	EXPECT_EQ(k.gate.inside, 0u) << "a waiting submitter is not counted";
	k.open();
	submitter.join();
	EXPECT_TRUE(done.load())
		<< "the submission went on after the wake: the client's work is not dropped";
}

TEST(GM20BSleepGate, TheCloserWaitsForASubmissionInProgress)
{
	Kernel k;
	k.enter();
	std::atomic<bool> drained{false};
	std::thread closer([&] {
		drained = k.close(std::chrono::seconds(5));
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_FALSE(drained.load()) << "the GPU went down under a submission";
	k.leave();
	closer.join();
	EXPECT_TRUE(drained.load());
	k.open();
}

TEST(GM20BSleepGate, ACallerThatNeverLeavesFailsTheBoundedDrain)
{
	Kernel k;
	k.enter();
	EXPECT_FALSE(k.close(std::chrono::milliseconds(20)))
		<< "the kernel panics here with the count still inside";
	k.leave();
	EXPECT_TRUE(tgm_gate_drained(&k.gate));
	k.open();
}

TEST(GM20BSleepPlan, EachStage)
{
	EXPECT_EQ(tgm_sleep_plan(0, 0), TGM_SLEEP_NOTHING)
		<< "switch-no-gpu, or M1 failed before the power-up";
	EXPECT_EQ(tgm_sleep_plan(3, 0), TGM_SLEEP_NOTHING);
	EXPECT_EQ(tgm_sleep_plan(0, 1), TGM_SLEEP_POWER)
		<< "M1 failed after the power-up (chip ID)";
	EXPECT_EQ(tgm_sleep_plan(1, 1), TGM_SLEEP_POWER) << "stage 1 only, or M2 failed";
	EXPECT_EQ(tgm_sleep_plan(2, 1), TGM_SLEEP_FULL) << "M2's nvif client exists";
	EXPECT_EQ(tgm_sleep_plan(3, 1), TGM_SLEEP_FULL);
}
}  // namespace
