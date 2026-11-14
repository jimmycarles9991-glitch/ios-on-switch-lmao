// SPDX-License-Identifier: MIT
// deadline.c and hold.c together over whole display periods, three slow app channels on one runlist: what one period
// costs in locks (the deadline's and the hold's own, nvkm's chid, group and channel spinlocks, the interrupt work loop's
// gate), read-modify-writes, thread-call schedules and runs; and the order a frame end, the holds, a preempt's check,
// the present and the window's expiry keep, with a present before the holds, a frame still in flight at the vblank, a
// kept window an app's kick opens, high work that needs a held channel, and system sleep in the middle.
#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <vector>
#include "drivers/TegraGM20B/test/gm20b_hold_stubs.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BSched.h"
}
#include "drivers/TegraGM20B/TegraGM20BHost.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"

extern "C" void tegra_gm20b_compositor_vsync(uint64_t stamp, uint32_t lead_us, uint64_t interval);

extern "C" {
struct switch_gpu_busy tgm_busy;
}

namespace
{
constexpr u64 kMs = 1000000, kPeriod = 16666667, kLatency = 5 * kMs, kStatusReads = 3;
constexpr unsigned kChids = 512, kApps[] = {11, 12, 13};

struct Counts {
	unsigned driverLocks;  // dl.lock, hold.lock
	unsigned chidLocks;    // nvkm's chid->lock and cgrp->lock (chan_get)
	unsigned chanLocks;    // nvkm's chan->lock
	unsigned mutexes;      // runl->mutex (a hold's preempt)
	unsigned gates;        // the interrupt work loop's gate, taken or tried
	unsigned rmws;
	unsigned schedules, cancels, runs;
	unsigned statusReads, ccsr;
};

Counts c;
u64 now;
int resident = TGM_VICTIM_NONE;
bool gateBusy;
unsigned spinDepth, gateDepth;
u32 seqs[kChids];
int stopped[kChids];

struct Work {
	void (*fn)(void *);
	void *arg;
	bool pending;
	u64 at;
};

std::vector<Work *> works;

nvkm_device device;
nvkm_fifo fifo;
nvkm_runl runl;
nvkm_chid chids;
nvkm_timer timer;
nvkm_chan_func chanFunc;
nvkm_cgrp cgrps[kChids];
nvkm_chan chans[kChids];
bool present[kChids];

bool isChanLock(const spinlock_t *l)
{
	return reinterpret_cast<const char *>(l) >= reinterpret_cast<const char *>(chans) &&
	       reinterpret_cast<const char *>(l) < reinterpret_cast<const char *>(chans + kChids);
}

void mustNotBlock(const char *what)
{
	if (spinDepth)
		ADD_FAILURE() << what << " under a spinlock";
}

void chanStop(nvkm_chan *chan)
{
	c.ccsr += 2;  // nvkm_mask: CCSR read, ENABLE_CLR write
	EXPECT_FALSE(stopped[chan->id]) << "channel " << chan->id << " stopped twice";
	stopped[chan->id] = 1;
}

void chanStart(nvkm_chan *chan)
{
	c.ccsr += 2;
	EXPECT_TRUE(stopped[chan->id]) << "channel " << chan->id << " started while running";
	stopped[chan->id] = 0;
}

Work *earliestDue(u64 by)
{
	Work *next = nullptr;
	for (Work *w : works) {
		if (w->pending && w->at <= by && (!next || w->at < next->at))
			next = w;
	}
	return next;
}

// Time moves on to `to`, each thread call running at its time.
void advance(u64 to)
{
	ASSERT_GE(to, now) << "time going back";
	for (Work *w = earliestDue(to); w; w = earliestDue(to)) {
		if (w->at > now)
			now = w->at;
		w->pending = false;
		++c.runs;
		w->fn(w->arg);
	}
	now = to;
}

// The interrupt pass: an action on the work loop, inside its gate.
template <typename F> void pass(F f)
{
	++gateDepth;
	f();
	--gateDepth;
}

void appWork(unsigned chid, bool on)
{
	if (on) {
		switch_gpu_busy_kick(&tgm_busy, chid, ++seqs[chid]);
	} else {
		switch_gpu_busy_done(&tgm_busy, chid, seqs[chid]);
	}
}

// A compositor submission queued behind the app channel, its start stamp `latency` after the ask
void sample(unsigned chid, u64 latency)
{
	const u64 asked = now;
	tgm_hold_asked(chid, asked);
	pass([&] {
		tgm_hold_sampled(chid, asked, asked + latency);
	});
}

unsigned held()
{
	unsigned n = 0;
	for (unsigned chid : kApps)
		n += stopped[chid];
	return n;
}

void frameEnd()
{
	tegra_gm20b_compositor_vsync(now, 0, kPeriod);
}

void presentCompleted(u64 queued)
{
	pass([&] {
		tgm_deadline_presented(queued, 1);
	});
}

void report(const char *what)
{
	std::printf("%s: %u deadline/hold locks, %u chid/cgrp locks, %u channel locks, %u mutexes, "
		    "%u gates, "
		    "%u RMWs, %u schedules, %u cancels, %u thread-call runs, %u status reads, %u "
		    "CCSR accesses\n",
		    what, c.driverLocks, c.chidLocks, c.chanLocks, c.mutexes, c.gates, c.rmws,
		    c.schedules, c.cancels, c.runs, c.statusReads, c.ccsr);
}
}  // namespace

extern "C" {
void spin_lock_irqsave_stub(spinlock_t *l)
{
	EXPECT_EQ(l->held, 0) << "a spinlock taken twice";
	l->held = 1;
	++spinDepth;
	if (isChanLock(l)) {
		++c.chanLocks;
	} else {
		++c.driverLocks;
	}
}

void spin_unlock_irqrestore_stub(spinlock_t *l)
{
	l->held = 0;
	--spinDepth;
}

void mutex_lock(struct mutex *m)
{
	mustNotBlock("a mutex");
	++c.mutexes;
	m->held = 1;
}

void mutex_unlock(struct mutex *m)
{
	m->held = 0;
}

int mutex_trylock(struct mutex *m)
{
	mustNotBlock("a mutex");
	++c.mutexes;
	if (m->held)
		return 0;
	m->held = 1;
	return 1;
}

int test_and_clear_bit(unsigned long nr, unsigned long *addr)
{
	++c.rmws;
	const int was = (int)((addr[nr / 64] >> (nr % 64)) & 1);
	addr[nr / 64] &= ~(1UL << (nr % 64));
	return was;
}

int test_and_set_bit(unsigned long nr, unsigned long *addr)
{
	++c.rmws;
	const int was = (int)((addr[nr / 64] >> (nr % 64)) & 1);
	addr[nr / 64] |= 1UL << (nr % 64);
	return was;
}

void atomic_inc(atomic_t *v)
{
	++c.rmws;
	++*v;
}

int atomic_dec_and_test(atomic_t *v)
{
	++c.rmws;
	return --*v == 0;
}

int proc_selfpid(void)
{
	return 1;
}

nvkm_chan *nvkm_runl_chan_get_chid(nvkm_runl *, int id, unsigned long *)
{
	c.chidLocks += 2;
	return id >= 0 && (unsigned)id < kChids && present[id] ? &chans[id] : nullptr;
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
	return now;
}

int tgm_sched_resident(nvkm_device *)
{
	c.statusReads += kStatusReads;
	return resident;
}

u64 nvxnu_nanotime(void)
{
	return now;
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
	mustNotBlock("an allocation");
	works.push_back(new Work{fn, arg, false, 0});
	return works.back();
}

void nvxnu_work_schedule(void *work)
{
	++c.schedules;
	static_cast<Work *>(work)->pending = true;
	static_cast<Work *>(work)->at = now;
}

void nvxnu_work_schedule_after(void *work, u64 ns)
{
	++c.schedules;
	static_cast<Work *>(work)->pending = true;
	static_cast<Work *>(work)->at = now + ns;
}

void nvxnu_work_cancel(void *work)
{
	++c.cancels;
	static_cast<Work *>(work)->pending = false;
}

void nvxnu_work_cancel_wait(void *work)
{
	mustNotBlock("a wait for a thread call");
	static_cast<Work *>(work)->pending = false;
}

void nvxnu_pass_run(void (*fn)(void *), void *arg)
{
	mustNotBlock("the work loop's gate");
	++c.gates;
	++gateDepth;
	fn(arg);
	--gateDepth;
}

int nvxnu_pass_try(void (*fn)(void *), void *arg)
{
	mustNotBlock("the work loop's gate");
	++c.gates;
	if (gateBusy && !gateDepth)
		return 0;
	++gateDepth;
	fn(arg);
	--gateDepth;
	return 1;
}
}

class DisplayPeriod : public ::testing::Test
{
protected:
	static void SetUpTestSuite()
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
		now = 1000 * kMs;
		tgm_hold_init();
		tgm_deadline_init(&device);
		pass([] {
			tgm_deadline_clock(now, now);
			tgm_deadline_clock(now + 200 * kMs, now + 200 * kMs);
		});
		for (unsigned chid : kApps) {
			cgrps[chid].runl = &runl;
			chans[chid].id = (int)chid;
			chans[chid].cgrp = &cgrps[chid];
			chans[chid].func = &chanFunc;
			present[chid] = true;
			switch_gpu_busy_open(&tgm_busy, chid, 0);
			switch_gpu_busy_level(&tgm_busy, chid, 1);
			tgm_hold_track(&device, chid, 1);
		}
	}

	void SetUp() override
	{
		advance(now + 20 * kPeriod);
		resident = TGM_VICTIM_NONE;
		gateBusy = false;
		for (unsigned chid : kApps) {
			appWork(chid, false);
			sample(chid, kLatency);
			ASSERT_FALSE(stopped[chid]);
		}
		c = {};
	}

	void TearDown() override
	{
		EXPECT_EQ(spinDepth, 0u);
		EXPECT_EQ(gateDepth, 0u);
	}
};

// One display period with the window opened at the frame end: the holds at their time (the channel on the GPU
// preempted), the preempt's check, the compositor's kick, its present before the vblank, the vblank. Each held channel
// is stopped and started under its own nvkm lock and nothing else: no lock of the deadline's or the hold's, no nvkm chid
// or group spinlock, no read-modify-write.
TEST_F(DisplayPeriod, AWindowOpenedAtTheFrameEnd)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 start = now;
	const u64 target = start + kPeriod;
	const u64 holdAt = target - kLatency;
	frameEnd();
	advance(now);
	EXPECT_EQ(held(), 0u) << "held before their time";
	resident = (int)kApps[0];
	advance(holdAt);
	EXPECT_EQ(held(), 3u);
	resident = TGM_VICTIM_NONE;
	tgm_deadline_kicked(1, 0);
	advance(holdAt + kLatency / 2);
	advance(target - kMs);
	presentCompleted(start + kMs);
	EXPECT_EQ(held(), 0u) << "the present released no hold";
	advance(target);
	EXPECT_EQ(held(), 0u);
	report("window at the frame end");
	EXPECT_EQ(c.driverLocks, 0u) << "dl.lock and hold.lock in a display period";
	EXPECT_EQ(c.chidLocks, 0u) << "nvkm's chid and group spinlocks for the held channels";
	EXPECT_EQ(c.rmws, 0u);
	EXPECT_EQ(c.chanLocks, 6u)
		<< "each channel's own lock, once to hold it and once to release it";
	EXPECT_LE(c.gates, 3u) << "the frame end's, the holds' and the check's";
	EXPECT_LE(c.schedules + c.cancels, 4u);
	EXPECT_LE(c.runs, 2u) << "thread-call runs beyond the holds and the check";
	EXPECT_EQ(c.statusReads, 2 * kStatusReads)
		<< "the resident look at the holds and at the check";
}

// The deadline's stats line (debug kernels) counts what a period took in the work loop's gate: the frame end's run,
// the owner's timed runs for the holds and the check, and none woken.
TEST_F(DisplayPeriod, TheDeadlineLineCountsTheGate)
{
	char line[512];
	for (unsigned chid : kApps)
		appWork(chid, true);
	tgm_deadline_stats(line, sizeof(line));
	const u64 start = now;
	const u64 target = start + kPeriod;
	frameEnd();
	resident = (int)kApps[0];
	advance(target - kLatency);
	resident = TGM_VICTIM_NONE;
	tgm_deadline_kicked(1, 0);
	advance(target - kMs);
	presentCompleted(start + kMs);
	advance(target);
	tgm_deadline_stats(line, sizeof(line));
	const std::string text(line);
	EXPECT_NE(
		text.find(
			"1 windows (0 with no frame, 0 expired, 0 opened by an app's kick); in the "
			"work loop's gate: 1 frame ends, 2 owner runs timed, 0 woken"),
		std::string::npos)
		<< text;
}

// The frame end found no slow channel with work and kept the window; an app's kick after the holds' time opens it and
// holds them before its doorbell; the present releases them.
TEST_F(DisplayPeriod, AKeptWindowAnAppKickOpens)
{
	const u64 start = now;
	const u64 target = start + kPeriod;
	frameEnd();
	advance(now);
	EXPECT_EQ(c.schedules, 0u) << "a kept window schedules nothing";
	tgm_deadline_kicked(1, 0);
	advance(target - kLatency + kMs);
	appWork(kApps[0], true);
	EXPECT_EQ(tgm_deadline_app_kicked(), 1);
	EXPECT_EQ(held(), 3u) << "the holds due by the kick, before its doorbell";
	advance(target - kMs);
	presentCompleted(start + kMs);
	EXPECT_EQ(held(), 0u);
	advance(target);
	report("kept window an app's kick opens");
	EXPECT_EQ(c.driverLocks, 0u) << "dl.lock and hold.lock in a display period";
	EXPECT_EQ(c.chidLocks, 0u) << "nvkm's chid and group spinlocks for the held channels";
	EXPECT_EQ(c.rmws, 0u);
	EXPECT_EQ(c.chanLocks, 6u);
	EXPECT_LE(c.gates, 1u) << "the kick's";
	EXPECT_LE(c.schedules + c.cancels, 2u);
	EXPECT_EQ(c.runs, 0u) << "a thread call ran for a window its present closed";
	EXPECT_EQ(c.statusReads, 0u);
}

// The present came before any hold was due: nothing is held for that frame, then or later.
TEST_F(DisplayPeriod, APresentBeforeTheHoldsAreDue)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 start = now;
	const u64 target = start + kPeriod;
	frameEnd();
	advance(start + 2 * kMs);
	presentCompleted(start + kMs);
	advance(target + kMs);
	EXPECT_EQ(c.ccsr, 0u) << "a channel stopped for a frame already presented";
	EXPECT_EQ(held(), 0u);
}

// A frame still in flight at the window's vblank keeps its holds one more interval; then they go.
TEST_F(DisplayPeriod, AFrameStillInFlightAtTheVblank)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 start = now;
	const u64 target = start + kPeriod;
	frameEnd();
	advance(target - kLatency);
	ASSERT_EQ(held(), 3u);
	tgm_deadline_kicked(1, 0);
	advance(target + kMs);
	EXPECT_EQ(held(), 3u) << "released at the vblank with the compositor's frame in flight";
	advance(target + kPeriod + kMs);
	EXPECT_EQ(held(), 0u) << "held past the extra interval";
}

// A window the compositor never kicked in had no frame: it ends at its vblank.
TEST_F(DisplayPeriod, AWindowWithNoFrameEndsAtItsVblank)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 target = now + kPeriod;
	frameEnd();
	advance(target - kLatency);
	ASSERT_EQ(held(), 3u);
	advance(target + kMs);
	EXPECT_EQ(held(), 0u);
}

// System sleep with holds taken: they are released at once, and nothing touches a channel or a register until the wake.
TEST_F(DisplayPeriod, SystemSleepInTheMiddle)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 target = now + kPeriod;
	frameEnd();
	resident = (int)kApps[1];
	advance(target - kLatency);
	ASSERT_EQ(held(), 3u);
	tgm_deadline_suspend();
	EXPECT_EQ(held(), 0u) << "a hold kept across system sleep";
	const unsigned ccsr = c.ccsr;
	const unsigned reads = c.statusReads;
	frameEnd();
	advance(target + 2 * kPeriod);
	EXPECT_EQ(c.ccsr, ccsr) << "a channel touched after the suspend";
	EXPECT_EQ(c.statusReads, reads) << "a register read after the suspend";
	tgm_deadline_resume();
	const u64 next = now + kPeriod;
	frameEnd();
	advance(next - kLatency);
	EXPECT_EQ(held(), 3u) << "no window after the wake";
	advance(next + 2 * kPeriod);
	EXPECT_EQ(held(), 0u);
}

// A present before any app kicked ends the window the frame end kept: a later kick opens nothing.
TEST_F(DisplayPeriod, APresentEndsTheKeptWindow)
{
	const u64 start = now;
	frameEnd();
	tgm_deadline_kicked(1, 0);
	advance(start + 2 * kMs);
	presentCompleted(start + kMs);
	advance(start + kPeriod - kLatency + kMs);
	appWork(kApps[0], true);
	EXPECT_EQ(tgm_deadline_app_kicked(), 0);
	EXPECT_EQ(held(), 0u);
}

// A newer frame end that keeps its window too replaces the one before (whose vblank is still ahead, with a lead): a
// kick after the old window's holds were due opens the new one, whose holds come at their own time.
TEST_F(DisplayPeriod, ANewerFrameEndReplacesTheKeptWindow)
{
	constexpr u32 kLeadUs = 2000;
	const u64 first = now;
	tegra_gm20b_compositor_vsync(first, kLeadUs, kPeriod);
	tgm_deadline_kicked(1, 0);
	advance(first + kPeriod);
	tegra_gm20b_compositor_vsync(now, kLeadUs, kPeriod);
	const u64 target = now + kLeadUs * 1000ULL + kPeriod;
	advance(now + kMs);
	appWork(kApps[0], true);
	tgm_deadline_app_kicked();
	advance(now);
	EXPECT_EQ(held(), 0u) << "held for the replaced window";
	advance(target - kLatency);
	EXPECT_EQ(held(), 3u);
	advance(target + 2 * kPeriod);
	EXPECT_EQ(held(), 0u);
}

// The app's kick finds a pass under way (the work loop's gate held): the kept window still opens, and its holds come
// once the pass is done; the present releases them.
TEST_F(DisplayPeriod, AnAppKickBesideAPass)
{
	const u64 start = now;
	const u64 target = start + kPeriod;
	frameEnd();
	tgm_deadline_kicked(1, 0);
	advance(target - kLatency + kMs);
	appWork(kApps[0], true);
	gateBusy = true;
	tgm_deadline_app_kicked();
	gateBusy = false;
	advance(now);
	EXPECT_EQ(held(), 3u);
	advance(target - kMs);
	presentCompleted(start + kMs);
	EXPECT_EQ(held(), 0u);
}

// High work that waits for a held channel releases it at once, and the channel is left alone until a present queued
// after that completes.
TEST_F(DisplayPeriod, HighWorkNeedsAHeldChannel)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 start = now;
	const u64 target = start + kPeriod;
	frameEnd();
	advance(target - kLatency);
	ASSERT_EQ(held(), 3u);
	tgm_hold_need(&device, kApps[2]);
	advance(now);
	EXPECT_FALSE(stopped[kApps[2]]) << "high work waits behind a held channel";
	EXPECT_TRUE(tgm_hold_needed(kApps[2]));
	advance(target - kMs);
	presentCompleted(start + kMs);
	EXPECT_TRUE(tgm_hold_needed(kApps[2])) << "a present queued before the need ended it";
	const u64 next = target + kPeriod;
	frameEnd();
	advance(next - kLatency);
	EXPECT_FALSE(stopped[kApps[2]]) << "held again before a present queued after the need";
	advance(next - kMs);
	presentCompleted(now);
	EXPECT_FALSE(tgm_hold_needed(kApps[2]));
	advance(next + kPeriod);
	EXPECT_EQ(held(), 0u);
}

// A held channel's teardown (or its rise to the high level) releases it, and no window holds it again.
TEST_F(DisplayPeriod, AnUntrackedChannelIsReleasedAndLeftAlone)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 target = now + kPeriod;
	frameEnd();
	advance(target - kLatency);
	ASSERT_EQ(held(), 3u);
	tgm_hold_track(&device, kApps[1], 0);
	advance(now);
	EXPECT_FALSE(stopped[kApps[1]]);
	advance(target + kPeriod + kMs);
	const u64 next = now + kPeriod;
	frameEnd();
	advance(next - kLatency);
	EXPECT_FALSE(stopped[kApps[1]]) << "an untracked channel held";
	EXPECT_EQ(held(), 2u);
	advance(next + 2 * kPeriod);
	tgm_hold_track(&device, kApps[1], 1);
}

// A frame end that finds a pass in the work loop's gate wakes the owner, which opens the window: the holds come at their
// time as before.
TEST_F(DisplayPeriod, AFrameEndBesideAPass)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 target = now + kPeriod;
	gateBusy = true;
	frameEnd();
	gateBusy = false;
	EXPECT_EQ(c.schedules, 1u) << "the owner's wake";
	advance(target - kLatency);
	EXPECT_EQ(held(), 3u);
	advance(target + 2 * kPeriod);
	EXPECT_EQ(held(), 0u);
}

// High work needs a held channel while a pass holds the gate: the owner's next run releases it.
TEST_F(DisplayPeriod, HighWorkNeedsAHeldChannelBesideAPass)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 target = now + kPeriod;
	frameEnd();
	advance(target - kLatency);
	ASSERT_EQ(held(), 3u);
	gateBusy = true;
	tgm_hold_need(&device, kApps[0]);
	EXPECT_TRUE(stopped[kApps[0]]) << "released outside the gate";
	gateBusy = false;
	advance(now);
	EXPECT_FALSE(stopped[kApps[0]]) << "high work waits behind a held channel";
	advance(target - kMs);
	presentCompleted(now);
	EXPECT_EQ(held(), 0u);
	EXPECT_FALSE(tgm_hold_needed(kApps[0]));
}

// A held channel rises to the high level while a pass holds the gate: the owner's next run releases it.
TEST_F(DisplayPeriod, AnUntrackedChannelBesideAPass)
{
	for (unsigned chid : kApps)
		appWork(chid, true);
	const u64 target = now + kPeriod;
	frameEnd();
	advance(target - kLatency);
	ASSERT_EQ(held(), 3u);
	gateBusy = true;
	tgm_hold_track(&device, kApps[2], 0);
	EXPECT_TRUE(stopped[kApps[2]]) << "released outside the gate";
	gateBusy = false;
	advance(now);
	EXPECT_FALSE(stopped[kApps[2]]);
	advance(target + 2 * kPeriod);
	EXPECT_EQ(held(), 0u);
	tgm_hold_track(&device, kApps[2], 1);
}

// A held channel is isolated (its kill under way) while a pass holds the gate: its hold ends without a release, at the
// owner's next run, and no present or teardown starts it; nvkm's recovery owns it.
TEST_F(DisplayPeriod, AnIsolationBesideAPass)
{
	const unsigned chid = kApps[1];
	for (unsigned app : kApps)
		appWork(app, true);
	const u64 start = now;
	const u64 target = start + kPeriod;
	frameEnd();
	advance(target - kLatency);
	ASSERT_EQ(held(), 3u);
	gateBusy = true;
	tgm_hold_drop(chid);
	gateBusy = false;
	advance(now + kMs);
	const u64 heldNs = tgm_hold_held_ns(chid, now);
	advance(target - kMs);
	EXPECT_EQ(tgm_hold_held_ns(chid, now), heldNs) << "an isolated channel still counted held";
	presentCompleted(start + kMs);
	EXPECT_TRUE(stopped[chid]) << "the hold started a channel nvkm's recovery owns";
	EXPECT_EQ(held(), 1u);
	tgm_hold_forget(chid);
	EXPECT_TRUE(stopped[chid]);
	stopped[chid] = 0;
	chans[chid].blocked = 0;
	tgm_hold_track(&device, chid, 1);
}
