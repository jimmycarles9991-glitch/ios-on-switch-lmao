// TegraGM20BRail.h: when the GPU's rail goes down and comes back up around the display's power; and gpu_power.c's rail
// protocol on threads (tgm_rail_down, tgm_gpu_lock, tgm_gate_enter's railed path) with a std::mutex and condition variables in
// place of lck_mtx and thread_wakeup.
#include "drivers/TegraGM20B/TegraGM20BRail.h"
#include "drivers/TegraGM20B/TegraGM20BSleepGate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

TEST(Gm20bRail, DownOnlyWithTheDisplayOffAndTheGpuIdle)
{
	EXPECT_EQ(tgm_rail_next(1, 0, 1, 0, 0), TGM_RAIL_DOWN);
	EXPECT_EQ(tgm_rail_next(1, 0, 0, 0, 0), TGM_RAIL_STAY) << "the last frames finish first";
	EXPECT_EQ(tgm_rail_next(1, 1, 1, 0, 0), TGM_RAIL_STAY)
		<< "a lit display keeps the GPU however idle";
}

TEST(Gm20bRail, UpOnTheThreadCallOnlyForTheDisplay)
{
	EXPECT_EQ(tgm_rail_next(0, 1, 0, 0, 0), TGM_RAIL_UP);
	EXPECT_EQ(tgm_rail_next(0, 0, 0, 0, 0), TGM_RAIL_STAY)
		<< "work wakes the GPU itself, in the caller at gpu.lock";
	EXPECT_EQ(tgm_rail_next(0, 0, 1, 1, 0), TGM_RAIL_STAY);
}

TEST(Gm20bRail, SystemSleepHoldsTheRailWherever)
{
	for (int up = 0; up < 2; ++up)
		for (int on = 0; on < 2; ++on)
			for (int idle = 0; idle < 2; ++idle)
				for (int held = 0; held < 2; ++held)
					EXPECT_EQ(tgm_rail_next(up, on, idle, held, 1),
						  TGM_RAIL_STAY);
}

TEST(Gm20bRail, ADemandHoldsTheGpuUpBeforeItGoesBackDown)
{
	EXPECT_EQ(tgm_rail_next(1, 0, 1, 1, 0), TGM_RAIL_STAY);
	EXPECT_EQ(tgm_rail_next(1, 0, 1, 0, 0), TGM_RAIL_DOWN) << "once the hold runs out";
}

TEST(Gm20bRail, TheHoldDoublesWithEachDemandUpToItsCap)
{
	EXPECT_EQ(tgm_rail_hold_ns(0), 0u)
		<< "the first rail-down after the display goes off waits only for idle";
	EXPECT_EQ(tgm_rail_hold_ns(1), 1000000000ull);
	EXPECT_EQ(tgm_rail_hold_ns(2), 2000000000ull);
	EXPECT_EQ(tgm_rail_hold_ns(7), 64000000000ull);
	EXPECT_EQ(tgm_rail_hold_ns(8), 64000000000ull);
	EXPECT_EQ(tgm_rail_hold_ns(UINT32_MAX), 64000000000ull);
}

// gpu_bringup.c's GPU time across rail cycles: PTIMER runs on the GPU's own clock (ahead of ktime here), restarts from ktime at
// every wake (nvkm_timer_init, then stamp_reseed), and tgm_timestamp_ns answers ktime while the GPU is down.
struct GpuTime {
	uint64_t ktime = 1000000000, ptimer = 1000000000, last = 0;
	bool railed = false;

	void run(uint64_t ns, uint64_t gpuAhead)
	{
		ktime += ns;
		if (!railed)
			ptimer += ns + gpuAhead;
	}

	uint64_t stamp()  // tgm_timestamp_ns
	{
		return last = tgm_stamp_monotonic(last, railed ? ktime : ptimer);
	}

	void down()  // sleep_nvkm's read of PTIMER before the power-down
	{
		last = tgm_stamp_monotonic(last, ptimer);
		railed = true;
	}

	void up()  // wake_m1: nvkm_timer_init, then stamp_reseed
	{
		ptimer = ktime;
		if (const uint64_t floor = tgm_stamp_reseed(last, ptimer))
			ptimer = floor;
		railed = false;
	}
};

TEST(Gm20bRail, GpuTimeNeverGoesBackwardsAcrossRailCycles)
{
	GpuTime t;
	uint64_t prev = 0, gpuWritten = 0;
	for (int cycle = 0; cycle < 50; cycle++) {
		for (int i = 0; i < 20; i++) {
			t.run(1000000, 20000);  // 1 ms of ktime, 2% fast on the GPU
			const uint64_t s = t.stamp();
			ASSERT_GE(s, prev)
				<< "cycle " << cycle << ": a GPU time handed out went backwards";
			prev = s;
			ASSERT_GE(t.ptimer, gpuWritten)
				<< "cycle " << cycle
				<< ": PTIMER (a semaphore's stamp) went backwards";
			gpuWritten = t.ptimer;
		}
		t.down();
		for (int i = 0; i < 5; i++) {
			t.run(100000, 0);
			const uint64_t s = t.stamp();
			ASSERT_GE(s, prev) << "cycle " << cycle
					   << ": railed, ktime behind the last PTIMER read";
			prev = s;
		}
		t.up();
		ASSERT_GT(t.ptimer, gpuWritten)
			<< "cycle " << cycle << ": PTIMER reseeded at or behind its last value";
		ASSERT_GE(t.ptimer, prev)
			<< "cycle " << cycle << ": PTIMER reseeded behind a time handed out";
	}
}

TEST(Gm20bRail, PtimerFollowsKtimeOnceKtimeIsAhead)
{
	EXPECT_EQ(tgm_stamp_monotonic(5, 9), 9u);
	EXPECT_EQ(tgm_stamp_monotonic(9, 5), 9u);
	EXPECT_EQ(tgm_stamp_monotonic(7, 7), 7u);
	EXPECT_EQ(tgm_stamp_reseed(5, 9), 0u);
	EXPECT_EQ(tgm_stamp_reseed(9, 9), 10u);
	EXPECT_EQ(tgm_stamp_reseed(9, 5), 10u);
}

// gpu_bringup.c: tgm_gpu.lock (`gpuLock`, owned by whoever takes it, as lck_mtx is), tgm_gate_lock with the gate's two events, and
// tgm_gpu.railed. tgm_rail_up_locked runs in whichever caller finds the GPU down, under the tgm_gpu.lock it took itself.
struct RailKernel {
	tgm_gate gate = {};
	std::mutex gateLock, gpuLock;
	std::condition_variable shutCv, insideCv;
	std::atomic<int> railed{0};
	std::atomic<bool> powered{true};
	std::atomic<int> wakes{0}, downs{0};

	void railUpLocked()
	{
		wakes++;
		powered = true;
		{
			std::lock_guard<std::mutex> hold(gateLock);
			tgm_gate_open(&gate);
			shutCv.notify_all();
		}
		railed = 0;
	}

	// tgm_gpu_lock: returns holding gpuLock, as the kernel's does (the analysis cannot follow a lock across calls)
	void lockGpu() __attribute__((no_thread_safety_analysis))
	{
		gpuLock.lock();
		if (railed.load())
			railUpLocked();
	}

	void unlockGpu() __attribute__((no_thread_safety_analysis))
	{
		gpuLock.unlock();
	}

	void enter()  // tgm_gate_enter
	{
		tgm_gate_entry entry;
		while ((entry = tgm_gate_try_enter(&gate)) != TGM_GATE_IN) {
			if (railed.load()) {
				if (entry == TGM_GATE_SHUT_WAKE) {
					std::lock_guard<std::mutex> hold(gateLock);
					insideCv.notify_all();
				}
				lockGpu();
				unlockGpu();
				continue;
			}
			std::unique_lock<std::mutex> hold(gateLock);
			if (entry == TGM_GATE_SHUT_WAKE)
				insideCv.notify_all();
			shutCv.wait(hold, [&] {
				return !tgm_gate_is_shut(&gate) || railed.load();
			});
		}
	}

	void leave()  // gate_leave
	{
		if (tgm_gate_leave(&gate)) {
			std::lock_guard<std::mutex> hold(gateLock);
			insideCv.notify_all();
		}
	}

	void
	down()  // tgm_rail_down: sleep_gate, sleep_fences, tgm_gpu.lock, the power-down, railed, the waiters told
	{
		{
			std::unique_lock<std::mutex> hold(gateLock);
			tgm_gate_close(&gate);
			insideCv.wait(hold, [&] {
				return tgm_gate_drained(&gate) != 0;
			});
		}
		gpuLock.lock();
		powered = false;
		railed = 1;
		downs++;
		gpuLock.unlock();
		std::lock_guard<std::mutex> hold(gateLock);
		shutCv.notify_all();
	}
};

TEST(Gm20bRail, EveryRailDownIsWokenOnceByAnyCallerAndNothingTouchesTheGpuWhileDown)
{
	constexpr int kAtGate = 4, kAtLock = 3, kDowns = 200;
	RailKernel k;
	std::atomic<int> violations{0};
	std::atomic<long> calls{0};
	std::atomic<bool> stop{false};
	auto work = [&] {
		if (!k.powered.load())
			violations++;
		std::this_thread::yield();
		calls++;
	};
	std::vector<std::thread> threads;
	for (int i = 0; i < kAtGate; i++)  // tgm_chan_exec, tgm_chan_new
		threads.emplace_back([&] {
			while (!stop.load()) {
				k.enter();
				work();
				k.leave();
			}
		});
	for (int i = 0; i < kAtLock; i++)  // the nvif calls under tgm_gpu.lock
		threads.emplace_back([&] {
			while (!stop.load()) {
				k.lockGpu();
				work();
				k.unlockGpu();
				std::this_thread::yield();
			}
		});
	for (int d = 0; d < kDowns; d++) {
		k.down();
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (k.railed.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		ASSERT_EQ(k.railed.load(), 0) << "rail-down " << d << ": no caller woke the GPU";
		ASSERT_EQ(k.wakes.load(), d + 1)
			<< "rail-down " << d << ": one wake each, by whoever came first";
	}
	stop = true;
	for (auto &t : threads)
		t.join();
	EXPECT_EQ(violations.load(), 0) << "GPU work while its rail was down";
	EXPECT_EQ(k.downs.load(), kDowns);
	EXPECT_GT(calls.load(), kDowns);
}

TEST(Gm20bRail, ACallerAtTheShutGateWakesTheGpuItself)
{
	RailKernel k;
	k.down();
	std::atomic<bool> done{false};
	std::thread submitter([&] {
		k.enter();
		done = k.powered.load();
		k.leave();
	});
	submitter.join();
	EXPECT_TRUE(done.load()) << "the submission went on with the GPU up";
	EXPECT_EQ(k.wakes.load(), 1);
	EXPECT_FALSE(tgm_gate_is_shut(&k.gate));
}

TEST(Gm20bRail, ACallerWaitingAtTheGateDuringTheDownWakesTheGpuOnceItIsDown)
{
	RailKernel k;
	{
		std::unique_lock<std::mutex> hold(k.gateLock);
		tgm_gate_close(&k.gate);
	}
	std::atomic<bool> done{false};
	std::thread submitter([&] {
		k.enter();
		done = k.powered.load();
		k.leave();
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_FALSE(done.load()) << "waits while the rail is on its way down";
	k.gpuLock.lock();
	k.powered = false;
	k.railed = 1;
	k.gpuLock.unlock();
	{
		std::lock_guard<std::mutex> hold(k.gateLock);
		k.shutCv.notify_all();
	}
	submitter.join();
	EXPECT_TRUE(done.load());
	EXPECT_EQ(k.wakes.load(), 1);
}

}  // namespace
