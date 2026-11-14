// SPDX-License-Identifier: MIT
// deadline.c and hold.c between threads, under ThreadSanitizer: the display's frame ends, the interrupt pass (the work
// loop's gate held: clock, progress, latency samples, presents, an isolation), the compositor's kicks and needs, three
// app channels' kicks into kept windows, a level change and a teardown, the stats lines, system sleep, and the owner's
// thread calls, each a thread of its own. Every channel is started as often as it was stopped, and runs at the end.
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include "drivers/TegraGM20B/test/gm20b_hold_stubs.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BSched.h"
}
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"

extern "C" void tegra_gm20b_compositor_vsync(uint64_t stamp, uint32_t lead_us, uint64_t interval);

extern "C" {
struct switch_gpu_busy tgm_busy;
}

namespace
{
using Clock = std::chrono::steady_clock;
constexpr u64 kMs = 1000000, kPeriod = 16666667, kLatency = 5 * kMs;
constexpr unsigned kChids = 512, kApps[] = {21, 22, 23};
constexpr int kPeriods = 48;

const Clock::time_point kStart = Clock::now();
std::atomic<int> resident{TGM_VICTIM_NONE};
std::atomic<bool> stopping{false};
std::recursive_mutex gate;

nvkm_device device;
nvkm_fifo fifo;
nvkm_runl runl;
nvkm_chid chids;
nvkm_timer timer;
nvkm_chan_func chanFunc;
nvkm_cgrp cgrps[kChids];
nvkm_chan chans[kChids];
int stopped[kChids];  // chan->lock's
std::atomic<unsigned> stops[kChids], starts[kChids];
std::atomic<bool> dropped{false};

u64 nowNs()
{
	return 1000 * kMs + static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
						     Clock::now() - kStart)
						     .count());
}

void sleepNs(u64 ns)
{
	std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

void chanStop(nvkm_chan *chan)
{
	EXPECT_FALSE(stopped[chan->id]);
	stopped[chan->id] = 1;
	stops[chan->id].fetch_add(1, std::memory_order_relaxed);
}

void chanStart(nvkm_chan *chan)
{
	EXPECT_TRUE(stopped[chan->id]);
	stopped[chan->id] = 0;
	starts[chan->id].fetch_add(1, std::memory_order_relaxed);
}

// A THREAD_CALL_OPTIONS_ONCE thread call: one run at a time, a schedule while it runs runs it again after.
struct Work {
	void (*fn)(void *);
	void *arg;
	std::mutex m;
	std::condition_variable cv;
	bool pending = false, running = false, quit = false;
	u64 at = 0;
	std::thread thread;

	Work(void (*f)(void *), void *a) : fn(f), arg(a)
	{
		thread = std::thread([this] {
			loop();
		});
	}

	void loop()
	{
		std::unique_lock<std::mutex> lock(m);
		for (;;) {
			if (quit)
				return;
			if (!pending) {
				cv.wait(lock);
				continue;
			}
			const u64 now = nowNs();
			if (at > now) {
				cv.wait_for(lock, std::chrono::nanoseconds(at - now));
				continue;
			}
			pending = false;
			running = true;
			lock.unlock();
			fn(arg);
			lock.lock();
			running = false;
			cv.notify_all();
		}
	}

	void schedule(u64 delay)
	{
		std::lock_guard<std::mutex> lock(m);
		pending = true;
		at = nowNs() + delay;
		cv.notify_all();
	}

	void cancel(bool wait)
	{
		std::unique_lock<std::mutex> lock(m);
		pending = false;
		while (wait && running)
			cv.wait(lock);
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(m);
			quit = true;
			cv.notify_all();
		}
		thread.join();
	}
};

std::vector<Work *> works;

template <typename F> void pass(F f)
{
	std::lock_guard<std::recursive_mutex> action(gate);
	f();
}
}  // namespace

extern "C" {
void spin_lock_irqsave_stub(spinlock_t *l)
{
	while (__atomic_exchange_n(&l->held, 1, __ATOMIC_ACQUIRE))
		std::this_thread::yield();
}

void spin_unlock_irqrestore_stub(spinlock_t *l)
{
	__atomic_store_n(&l->held, 0, __ATOMIC_RELEASE);
}

void mutex_lock(struct mutex *m)
{
	while (__atomic_exchange_n(&m->held, 1, __ATOMIC_ACQUIRE))
		std::this_thread::yield();
}

void mutex_unlock(struct mutex *m)
{
	__atomic_store_n(&m->held, 0, __ATOMIC_RELEASE);
}

int mutex_trylock(struct mutex *m)
{
	return !__atomic_exchange_n(&m->held, 1, __ATOMIC_ACQUIRE);
}

int test_and_clear_bit(unsigned long nr, unsigned long *addr)
{
	const unsigned long bit = 1UL << (nr % 64);
	return (__atomic_fetch_and(&addr[nr / 64], ~bit, __ATOMIC_SEQ_CST) & bit) != 0;
}

int test_and_set_bit(unsigned long nr, unsigned long *addr)
{
	const unsigned long bit = 1UL << (nr % 64);
	return (__atomic_fetch_or(&addr[nr / 64], bit, __ATOMIC_SEQ_CST) & bit) != 0;
}

void atomic_inc(atomic_t *v)
{
	__atomic_fetch_add(v, 1, __ATOMIC_SEQ_CST);
}

int atomic_dec_and_test(atomic_t *v)
{
	return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST) == 0;
}

int proc_selfpid(void)
{
	return 1;
}

nvkm_chan *nvkm_runl_chan_get_chid(nvkm_runl *, int id, unsigned long *)
{
	return id >= 0 && (unsigned)id < kChids && chans[id].func ? &chans[id] : nullptr;
}

void nvkm_chan_put(nvkm_chan **chan, unsigned long)
{
	*chan = nullptr;
}

u32 nvkm_rd32(nvkm_device *, u32)
{
	return 0;
}

void nvkm_wr32(nvkm_device *, u32, u32)
{
}

u64 nvkm_timer_read(nvkm_timer *)
{
	return nowNs();
}

int tgm_sched_resident(nvkm_device *)
{
	return resident.load(std::memory_order_relaxed);
}

u64 nvxnu_nanotime(void)
{
	return nowNs();
}

u64 nvxnu_abstime_to_ns(u64 t)
{
	return t;
}

void nvxnu_log(const char *, ...)
{
}

void nvxnu_panic(const char *, ...)
{
	ADD_FAILURE() << "panic";
}

void *nvxnu_work_alloc(void (*fn)(void *), void *arg)
{
	works.push_back(new Work(fn, arg));
	return works.back();
}

void nvxnu_work_schedule(void *work)
{
	static_cast<Work *>(work)->schedule(0);
}

void nvxnu_work_schedule_after(void *work, u64 ns)
{
	static_cast<Work *>(work)->schedule(ns);
}

void nvxnu_work_cancel(void *work)
{
	static_cast<Work *>(work)->cancel(false);
}

void nvxnu_work_cancel_wait(void *work)
{
	static_cast<Work *>(work)->cancel(true);
}

void nvxnu_pass_run(void (*fn)(void *), void *arg)
{
	std::lock_guard<std::recursive_mutex> action(gate);
	fn(arg);
}

int nvxnu_pass_try(void (*fn)(void *), void *arg)
{
	if (!gate.try_lock())
		return 0;
	fn(arg);
	gate.unlock();
	return 1;
}
}

TEST(PeriodThreads, EveryContextAtOnce)
{
	INIT_LIST_HEAD(&fifo.runls);
	list_add_tail(&runl.head, &fifo.runls);
	chids.nr = kChids;
	runl.chid = &chids;
	runl.fifo = &fifo;
	device.fifo = &fifo;
	device.timer = &timer;
	chanFunc.stop = chanStop;
	chanFunc.start = chanStart;
	for (unsigned chid : kApps) {
		cgrps[chid].runl = &runl;
		chans[chid].id = (int)chid;
		chans[chid].cgrp = &cgrps[chid];
		chans[chid].func = &chanFunc;
		switch_gpu_busy_open(&tgm_busy, chid, 0);
		switch_gpu_busy_level(&tgm_busy, chid, 1);
	}
	tgm_hold_init();
	tgm_deadline_init(&device);
	const u64 t0 = nowNs();
	pass([&] {
		tgm_deadline_clock(t0, t0);
		tgm_deadline_clock(t0 + 200 * kMs, t0 + 200 * kMs);
	});
	const auto sample = [](unsigned chid) {
		const u64 asked = nowNs();
		tgm_hold_asked(chid, asked);
		pass([&] {
			tgm_hold_sampled(chid, asked, asked + kLatency);
		});
	};
	for (unsigned chid : kApps) {
		tgm_hold_track(&device, chid, 1);
		sample(chid);
	}
	std::atomic<u64> frameEnd{0};
	std::atomic<unsigned> seqs[kChids] = {};
	std::vector<std::thread> threads;

	threads.emplace_back([&] {
		for (int i = 0; i < kPeriods && !stopping; i++) {
			const u64 now = nowNs();
			frameEnd.store(now);
			tegra_gm20b_compositor_vsync(now, 0, kPeriod);
			sleepNs(kPeriod);
		}
		stopping = true;
	});
	threads.emplace_back([&] {
		std::mt19937 rng(1);
		while (!stopping) {
			sleepNs(kMs + rng() % (4 * kMs));
			pass([&] {
				const u64 now = nowNs();
				tgm_deadline_clock(now, now);
				tgm_deadline_progress(2 * kMs, 0);
				for (unsigned chid : kApps) {
					if (rng() % 3 == 0)
						switch_gpu_busy_done(&tgm_busy, chid,
								     seqs[chid].load());
					tgm_hold_released(chid, now, 1);
				}
				if (rng() % 2 == 0)
					tgm_deadline_presented(frameEnd.load() + kMs, 1);
				if (!dropped && rng() % 32 == 0) {
					// nvkm's recovery takes it (errored): the hold's own block stays with it
					spin_lock_irqsave_stub(&chans[kApps[0]].lock);
					__atomic_store_n(&chans[kApps[0]].errored, 1,
							 __ATOMIC_RELAXED);
					spin_unlock_irqrestore_stub(&chans[kApps[0]].lock);
					dropped = true;
					tgm_hold_drop(kApps[0]);
				}
			});
		}
	});
	threads.emplace_back([&] {
		std::mt19937 rng(2);
		while (!stopping) {
			sleepNs(2 * kMs);
			tgm_deadline_kicked(1, rng() % 2);
			const unsigned app = kApps[rng() % 3];
			if (rng() % 20 == 0)
				tgm_hold_need(&device, app);
			tgm_hold_asked(app, nowNs());
			(void)tgm_hold_needed(app);
			resident.store(rng() % 2 ? (int)kApps[rng() % 3] : TGM_VICTIM_NONE);
		}
	});
	for (unsigned chid : kApps) {
		threads.emplace_back([&, chid] {
			std::mt19937 rng(chid);
			while (!stopping) {
				sleepNs(kMs + rng() % (6 * kMs));
				switch_gpu_busy_kick(&tgm_busy, chid, ++seqs[chid]);
				tgm_deadline_app_kicked();
			}
		});
	}
	threads.emplace_back([&] {
		while (!stopping) {
			sleepNs(37 * kMs);
			tgm_hold_track(&device, kApps[1], 0);
			sleepNs(5 * kMs);
			tgm_hold_track(&device, kApps[1], 1);
			sample(kApps[1]);
			sleepNs(23 * kMs);
			tgm_hold_forget(kApps[2]);
			tgm_hold_track(&device, kApps[2], 1);
			sample(kApps[2]);
		}
	});
	threads.emplace_back([&] {
		char line[1024];
		u64 latency = 0;
		unsigned turns = 0;
		while (!stopping) {
			sleepNs(50 * kMs);
			tgm_deadline_stats(line, sizeof(line));
			tgm_hold_stats(line, sizeof(line));
			for (unsigned chid : kApps) {
				tgm_hold_chan_stats(chid, &latency, &turns);
				(void)tgm_hold_held_ns(chid, nowNs());
			}
		}
	});
	threads.emplace_back([&] {
		sleepNs(kPeriods * kPeriod / 2);
		tgm_deadline_suspend();
		sleepNs(3 * kPeriod);
		tgm_deadline_resume();
	});
	for (auto &t : threads)
		t.join();
	tgm_deadline_suspend();
	for (unsigned chid : kApps)
		tgm_hold_forget(chid);
	for (Work *w : works)
		w->stop();
	unsigned held = 0;
	for (unsigned chid : kApps) {
		held += stops[chid].load();
		if (chid == kApps[0] && dropped) {
			EXPECT_EQ(stops[chid] - starts[chid], (unsigned)chans[chid].blocked);
			continue;
		}
		EXPECT_EQ(stops[chid].load(), starts[chid].load()) << "channel " << chid;
		EXPECT_FALSE(stopped[chid]) << "channel " << chid << " left held";
		EXPECT_EQ(chans[chid].blocked, 0) << "channel " << chid;
	}
	EXPECT_GT(held, 0u) << "nothing was held";
}
