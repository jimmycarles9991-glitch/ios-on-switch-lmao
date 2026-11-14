// SPDX-License-Identifier: MIT
/*
 * The hold (TegraGM20BSched.h): app channels kept off the GPU for the compositor's frame, each from its own preemption
 * latency before the compositor's latest start (deadline.c picks the times) until the present completes. A hold is
 * nvkm's own counted block (chan->blocked under chan->lock: gk104_chan_stop's ENABLE_CLR, gk104_chan_start's ENABLE_SET
 * once the count is back at zero), so it composes with nvkm's blocks for a teardown or a context bind; the runlist is
 * never rebuilt for it (nv50_runl_wait polls). The channel the GPU is on is preempted too, and leaves at its next
 * wait-for-idle: its preemption latency is learned from the compositor's start stamp after it (gpu_fenced.h), and brought down
 * when a check half that latency after a preempt finds it gone.
 *
 * Holds, releases and latencies are written in the GPU interrupt work loop's gate (deadline.c), which reaches each
 * channel through the nvkm channel its first submission found (hold.chans), so no nvkm chid or group lock is taken.
 * nvkm blocks, unblocks and errors channels on other threads under chan->lock, so a hold still takes it, and its count
 * in `blocked` is a plain load and store: every writer of `blocked` holds that lock. A first submission, high work that
 * needs a channel, an isolation and a rise to the high level write their own words outside the gate; a release one of
 * them needs is made in the gate at once if it is free, else by the deadline owner's next run.
 */
#include <core/device.h>
#include <engine/fifo.h>
#include <engine/fifo/runl.h>
#include <engine/fifo/cgrp.h>
#include <engine/fifo/chan.h>
#include <engine/fifo/chid.h>
#include <subdev/timer.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BHost.h"
#include "TegraGM20BSched.h"
#include "TegraGM20BSchedPolicy.h"
#include "SwitchGpuChanTable.h"
#include "deadline_stats.h"

#define HOLD_CHIDS 512 /* GM20B's channels (CCSR_CHANNEL__SIZE_1) */
#define HOLD_LATENCY_MAX TGM_INTERVAL_MAX_NS

static struct {
	/* the gate's; `slow` and `held` are also read outside it, so their words are stored whole */
	DECLARE_BITMAP(slow, HOLD_CHIDS); /* tracked, its latency worth a hold (tgm_hold_worth) */
	DECLARE_BITMAP(held, HOLD_CHIDS);
	DECLARE_BITMAP(held_last, HOLD_CHIDS); /* held in the last window, not yet given its turn */
	DECLARE_BITMAP(turned, HOLD_CHIDS);    /* given its turn this window */
	DECLARE_BITMAP(hopeless,
		       HOLD_CHIDS); /* stats: its hold put off to the next vblank this window */
	u32 latency[HOLD_CHIDS];    /* preemption latency, ns */
	u64 held_since[HOLD_CHIDS], held_total[HOLD_CHIDS];
	u64 closed;      /* when the last window released its holds */
	u64 unneed_upto; /* the last present's queue time: high work queued before it is done */
	/* counts that only grow (deadline_stats.h) */
	u32 turns[HOLD_CHIDS];
	u32 holds, preempts, busy, turned_n, hopeless_n, samples, checks;

	/* outside the gate */
	struct switch_gpu_chan_table
		chans; /* nvkm's channel: set by its first submission, cleared in the gate */
	u64 asked_pt[HOLD_CHIDS],
		asked_ns[HOLD_CHIDS]; /* our ask to leave the GPU not yet sampled: PTIMER, kernel clock */
	u64 need_ns[HOLD_CHIDS];  /* high work waits for it, since then (tgm_hold_need) */
	u8 tracked[HOLD_CHIDS];   /* gpu_exec.c's app channels */
	u8 isolating[HOLD_CHIDS]; /* gpu_hang.c is killing it (tgm_hold_drop) */
} hold;

void tgm_hold_init(void)
{
	static int once;
	unsigned int i = 0;

	if (once)
		return;
	once = 1;
	for (i = 0; i < HOLD_CHIDS; i++)
		hold.latency[i] = TGM_PREEMPT_MIN_NS;
}

/* The gate: a bit of a map that is read outside it */
static void word_set(unsigned long *map, unsigned int chid, int on)
{
	unsigned long *word = &map[chid / BITS_PER_LONG];
	const unsigned long bit = 1UL << (chid % BITS_PER_LONG);
	const unsigned long was = __atomic_load_n(word, __ATOMIC_RELAXED);

	__atomic_store_n(word, on ? was | bit : was & ~bit, __ATOMIC_RELAXED);
}

static int word_test(const unsigned long *map, unsigned int chid)
{
	return (int)((__atomic_load_n(&map[chid / BITS_PER_LONG], __ATOMIC_RELAXED) >>
		      (chid % BITS_PER_LONG)) &
		     1);
}

static int tracked(unsigned int chid)
{
	return __atomic_load_n(&hold.tracked[chid], __ATOMIC_ACQUIRE);
}

static int isolating(unsigned int chid)
{
	return __atomic_load_n(&hold.isolating[chid], __ATOMIC_ACQUIRE);
}

static int needed(unsigned int chid)
{
	return __atomic_load_n(&hold.need_ns[chid], __ATOMIC_ACQUIRE) >
	       __atomic_load_n(&hold.unneed_upto, __ATOMIC_ACQUIRE);
}

/* A channel's first submission: nvkm's channel, looked up once under nvkm's chid and group locks. It lives until the
 * teardown's tgm_hold_forget clears it in the gate. */
static struct nvkm_chan *chan_find(struct nvkm_device *device, unsigned int chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *chan = NULL;
	struct nvkm_chan *found = NULL;
	unsigned long flags = 0;

	nvkm_runl_foreach(runl, device->fifo)
	{
		if ((int)chid >= runl->chid->nr)
			continue;
		chan = nvkm_runl_chan_get_chid(runl, (int)chid, &flags);
		if (chan) {
			found = chan;
			nvkm_chan_put(&chan, flags);
			return found;
		}
	}
	return NULL;
}

static struct nvkm_chan *chan_of(unsigned int chid)
{
	return switch_gpu_chan_table_get(&hold.chans, chid);
}

/* The gate */
static void latency_set(unsigned int chid, u64 ns)
{
	WRITE_ONCE(hold.latency[chid], (u32)ns);
	word_set(hold.slow, chid, tgm_hold_worth(ns) && tracked(chid));
}

/* The gate */
static void held_end(unsigned int chid, u64 now)
{
	const u64 since = hold.held_since[chid];

	if (!test_bit(chid, hold.held))
		return;
	word_set(hold.held, chid, 0);
	WRITE_ONCE(hold.held_total[chid], hold.held_total[chid] + (now > since ? now - since : 0));
	WRITE_ONCE(hold.held_since[chid], 0);
}

/* The gate. The held bit is up before the look at `needed`, and tgm_hold_need stores its need before it looks at the
 * bit, so one of the two sees the other. */
static int chan_hold(unsigned int chid, u64 now)
{
	struct nvkm_chan *chan = chan_of(chid);
	unsigned long irq = 0;
	int held = 0;

	if (!chan)
		return 0;
	word_set(hold.held, chid, 1);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	spin_lock_irqsave(&chan->lock, irq);
	if (tgm_may_hold(TGM_LEVEL_FOREGROUND, tracked(chid), needed(chid),
			 atomic_read(&chan->errored), isolating(chid), atomic_read(&chan->cgrp->rc),
			 atomic_read(&chan->blocked))) {
		atomic_set(&chan->blocked, atomic_read(&chan->blocked) + 1);
		chan->func->stop(chan);
		held = 1;
	}
	spin_unlock_irqrestore(&chan->lock, irq);
	if (!held) {
		word_set(hold.held, chid, 0);
		return 0;
	}
	WRITE_ONCE(hold.held_since[chid], now ? now : 1);
	if (TGM_STATS)
		WRITE_ONCE(hold.holds, hold.holds + 1);
	return 1;
}

/* The gate. A channel errored or put into recovery since it was held stays blocked: nvkm's recovery owns it. */
static void chan_release(unsigned int chid, u64 now)
{
	struct nvkm_chan *chan = NULL;
	unsigned long irq = 0;
	int blocked = 0;

	if (!test_bit(chid, hold.held))
		return;
	held_end(chid, now);
	chan = chan_of(chid);
	if (!chan)
		return;
	spin_lock_irqsave(&chan->lock, irq);
	if (tgm_may_release(atomic_read(&chan->errored), isolating(chid),
			    atomic_read(&chan->cgrp->rc))) {
		blocked = atomic_read(&chan->blocked) - 1;
		atomic_set(&chan->blocked, blocked);
		if (!blocked)
			chan->func->start(chan);
	}
	spin_unlock_irqrestore(&chan->lock, irq);
}

/* The gate: a channel high work needs or no longer tracked is released, an isolated one forgotten unreleased. */
static void settle(unsigned int chid, u64 now)
{
	if (isolating(chid)) {
		held_end(chid, now);
		word_set(hold.slow, chid, 0);
		return;
	}
	if (!tracked(chid))
		word_set(hold.slow, chid, 0);
	if (test_bit(chid, hold.held) && (!tracked(chid) || needed(chid)))
		chan_release(chid, now);
}

static void settle_one(void *arg)
{
	settle(*(const unsigned int *)arg, nvxnu_nanotime());
}

/* Outside the gate, after the caller's words are stored */
static void settle_now(unsigned int chid)
{
	if (!nvxnu_pass_try(settle_one, &chid))
		tgm_deadline_wake();
}

void tgm_hold_settle(struct nvkm_device *device, uint64_t now)
{
	unsigned int i = 0;

	(void)device;
	for (i = 0; i < BITS_TO_LONGS(HOLD_CHIDS); i++) {
		unsigned long bits = hold.held[i] | hold.slow[i];

		while (bits) {
			const unsigned int chid =
				i * BITS_PER_LONG + (unsigned int)__builtin_ctzl(bits);

			bits &= bits - 1;
			settle(chid, now);
		}
	}
}

void tgm_hold_track(struct nvkm_device *device, unsigned int chid, int on)
{
	if (chid >= HOLD_CHIDS)
		return;
	if (on) {
		if (TGM_STATS)
			tgm_hold_owner(chid);
		if (!chan_of(chid))
			switch_gpu_chan_table_set(&hold.chans, chid, chan_find(device, chid));
		__atomic_store_n(&hold.tracked[chid], 1, __ATOMIC_RELEASE);
		return;
	}
	/* against chan_hold: it raises the held bit, then looks at tracked */
	__atomic_store_n(&hold.tracked[chid], 0, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (word_test(hold.held, chid) || word_test(hold.slow, chid))
		settle_now(chid);
}

static void forget(void *arg)
{
	const unsigned int chid = *(const unsigned int *)arg;

	settle(chid, nvxnu_nanotime());
	__clear_bit(chid, hold.held_last);
	__clear_bit(chid, hold.turned);
	__clear_bit(chid, hold.hopeless);
	WRITE_ONCE(hold.latency[chid], TGM_PREEMPT_MIN_NS);
	__atomic_store_n(&hold.asked_pt[chid], 0, __ATOMIC_RELAXED);
	__atomic_store_n(&hold.isolating[chid], 0, __ATOMIC_RELAXED);
	switch_gpu_chan_table_set(&hold.chans, chid, NULL);
}

void tgm_hold_forget(unsigned int chid)
{
	if (chid >= HOLD_CHIDS)
		return;
	__atomic_store_n(&hold.tracked[chid], 0, __ATOMIC_RELEASE);
	nvxnu_pass_run(forget, &chid);
}

void tgm_hold_drop(unsigned int chid)
{
	if (chid >= HOLD_CHIDS)
		return;
	__atomic_store_n(&hold.isolating[chid], 1, __ATOMIC_RELEASE);
	__atomic_store_n(&hold.tracked[chid], 0, __ATOMIC_RELEASE);
	settle_now(chid);
}

void tgm_hold_need(struct nvkm_device *device, unsigned int chid)
{
	const u64 now = nvxnu_nanotime();
	u64 was = 0;

	(void)device;
	if (chid >= HOLD_CHIDS)
		return;
	/* The compositor's and the present's kicks need channels from several threads: a need only moves forward, or a
	 * present queued between two needs would end the later one. */
	was = __atomic_load_n(&hold.need_ns[chid], __ATOMIC_RELAXED);
	while (was < now && !__atomic_compare_exchange_n(&hold.need_ns[chid], &was, now, false,
							 __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
	}
	/* against chan_hold: it raises the held bit, then looks at the need */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (word_test(hold.held, chid))
		settle_now(chid);
}

int tgm_hold_needed(unsigned int chid)
{
	return chid < HOLD_CHIDS && needed(chid);
}

void tgm_hold_unneed(uint64_t queued)
{
	if (queued > hold.unneed_upto)
		__atomic_store_n(&hold.unneed_upto, queued, __ATOMIC_RELEASE);
}

/* The gate: whether the window leaves `chid` alone for now, its one-period turn marked when it is owed one */
static int left_alone(unsigned int chid, const struct tgm_window *w)
{
	if (test_bit(chid, hold.held) || test_bit(chid, hold.turned) || needed(chid))
		return 1;
	if (!tgm_owed_turn((int)test_bit(chid, hold.held_last), w->now - hold.closed,
			   hold.latency[chid]))
		return 0;
	__set_bit(chid, hold.turned);
	__clear_bit(chid, hold.held_last);
	if (TGM_STATS) {
		WRITE_ONCE(hold.turned_n, hold.turned_n + 1);
		WRITE_ONCE(hold.turns[chid], hold.turns[chid] + 1);
	}
	return 1;
}

uint64_t tgm_hold_plan(const struct tgm_window *w)
{
	unsigned long chid = 0;
	u64 next = 0;
	u64 at = 0;

	for_each_set_bit(chid, hold.slow, HOLD_CHIDS)
	{
		if (left_alone((unsigned int)chid, w))
			continue;
		at = tgm_hold_lead_start(w->vblank, w->expected, hold.latency[chid]);
		at = at > w->now ? at : w->now;
		next = !next || at < next ? at : next;
	}
	return next;
}

uint64_t tgm_hold_due(struct nvkm_device *device, const struct tgm_window *w, int resident,
		      int *preempt)
{
	unsigned long chid = 0;
	u64 next = 0;
	u64 at = 0;
	u64 latency = 0;

	*preempt = -1;
	for_each_set_bit(chid, hold.slow, HOLD_CHIDS)
	{
		if (left_alone((unsigned int)chid, w))
			continue;
		latency = hold.latency[chid];
		if (resident == TGM_RESIDENT_LOOK &&
		    w->now >= tgm_hold_lead_start(w->vblank, w->expected, latency))
			resident = tgm_sched_resident(device);
		at = tgm_hold_time(w->vblank, w->interval, w->now, w->expected, latency,
				   (int)chid == resident);
		if (at > w->now) {
			if (TGM_STATS && at > w->vblank && !test_bit(chid, hold.hopeless)) {
				__set_bit(chid, hold.hopeless);
				WRITE_ONCE(hold.hopeless_n, hold.hopeless_n + 1);
			}
			next = !next || at < next ? at : next;
			continue;
		}
		if (chan_hold((unsigned int)chid, w->now) && (int)chid == resident)
			*preempt = (int)chid;
	}
	return next;
}

/* The gate's preempt and a high kick's both set the ask while it is 0: two that find it 0 in the same microseconds leave
 * the later, and that one sample comes out short by the gap. */
static void ask(unsigned int chid, u64 pt)
{
	if (__atomic_load_n(&hold.asked_pt[chid], __ATOMIC_RELAXED))
		return;
	__atomic_store_n(&hold.asked_ns[chid], nvxnu_nanotime(), __ATOMIC_RELAXED);
	__atomic_store_n(&hold.asked_pt[chid], pt, __ATOMIC_RELEASE);
}

int tgm_hold_preempt(struct nvkm_device *device, unsigned int chid)
{
	struct nvkm_chan *chan = chid < HOLD_CHIDS ? chan_of(chid) : NULL;
	struct nvkm_runl *runl = chan ? chan->cgrp->runl : NULL;
	u64 pt = 0;

	/* nvkm writes PFIFO_PREEMPT under runl->mutex and waits for it there; one of its own is in flight */
	if (!runl || !mutex_trylock(&runl->mutex))
		goto busy;
	if (nvkm_rd32(device, PFIFO_PREEMPT) & PFIFO_PREEMPT_PENDING) {
		mutex_unlock(&runl->mutex);
		goto busy;
	}
	nvkm_wr32(device, PFIFO_PREEMPT, chid);
	pt = nvkm_timer_read(device->timer);
	mutex_unlock(&runl->mutex);
	/* a kick's own preempt asked first: the latency runs from that ask */
	ask(chid, pt);
	if (TGM_STATS)
		WRITE_ONCE(hold.preempts, hold.preempts + 1);
	return 1;
busy:
	if (TGM_STATS)
		WRITE_ONCE(hold.busy, hold.busy + 1);
	return 0;
}

uint64_t tgm_hold_asked(unsigned int chid, uint64_t asked_pt)
{
	if (chid >= HOLD_CHIDS)
		return 0;
	if (asked_pt)
		ask(chid, asked_pt);
	return __atomic_load_n(&hold.asked_pt[chid], __ATOMIC_ACQUIRE);
}

void tgm_hold_sampled(unsigned int chid, uint64_t asked_pt, uint64_t start_pt)
{
	u64 sample = 0;

	if (chid >= HOLD_CHIDS)
		return;
	/* only the first submission behind an ask: later ones also waited for the compositor's own work before them */
	if (__atomic_load_n(&hold.asked_pt[chid], __ATOMIC_ACQUIRE) == asked_pt &&
	    start_pt > asked_pt)
		sample = tgm_deadline_pt_ns(start_pt - asked_pt);
	if (!sample)
		return;
	latency_set(chid, tgm_preempt_sampled(hold.latency[chid], sample, HOLD_LATENCY_MAX));
	__atomic_store_n(&hold.asked_pt[chid], 0, __ATOMIC_RELAXED);
	if (TGM_STATS)
		WRITE_ONCE(hold.samples, hold.samples + 1);
	if (TGM_STATS)
		tgm_hold_landed(chid, asked_pt, start_pt, sample);
}

void tgm_hold_check(struct nvkm_device *device, unsigned int chid)
{
	int resident = 0;

	if (chid >= HOLD_CHIDS || !__atomic_load_n(&hold.asked_pt[chid], __ATOMIC_RELAXED))
		return;
	resident = tgm_sched_resident(device);
	if (resident == (int)chid || resident == TGM_VICTIM_SWITCHING)
		return;
	latency_set(chid, tgm_preempt_left_early(hold.latency[chid]));
	__atomic_store_n(&hold.asked_pt[chid], 0, __ATOMIC_RELAXED);
	if (TGM_STATS)
		WRITE_ONCE(hold.checks, hold.checks + 1);
}

int tgm_hold_wanted(int pending)
{
	unsigned int i = 0;

	if (pending)
		return switch_gpu_busy_among(&tgm_busy, (const uint64_t *)hold.slow);
	for (i = 0; i < BITS_TO_LONGS(HOLD_CHIDS); i++) {
		if (__atomic_load_n(&hold.slow[i], __ATOMIC_RELAXED))
			return 1;
	}
	return 0;
}

uint64_t tgm_hold_latency(unsigned int chid)
{
	return chid < HOLD_CHIDS ? READ_ONCE(hold.latency[chid]) : TGM_PREEMPT_MIN_NS;
}

void tgm_hold_release(struct nvkm_device *device, uint64_t now)
{
	unsigned long chid = 0;

	(void)device;
	bitmap_copy(hold.held_last, hold.held, HOLD_CHIDS);
	for_each_set_bit(chid, hold.held_last, HOLD_CHIDS) chan_release((unsigned int)chid, now);
	bitmap_zero(hold.turned, HOLD_CHIDS);
	bitmap_zero(hold.hopeless, HOLD_CHIDS);
	/* an ask no submission queued behind (the channel left before the compositor came): no sample is coming for it */
	for (chid = 0; chid < HOLD_CHIDS; chid++) {
		if (__atomic_load_n(&hold.asked_pt[chid], __ATOMIC_RELAXED) &&
		    now - __atomic_load_n(&hold.asked_ns[chid], __ATOMIC_RELAXED) >
			    TGM_INTERVAL_MAX_NS)
			__atomic_store_n(&hold.asked_pt[chid], 0, __ATOMIC_RELAXED);
	}
	hold.closed = now;
}

uint64_t tgm_hold_held_ns(unsigned int chid, uint64_t now)
{
	u64 since = 0;
	u64 total = 0;

	if (chid >= HOLD_CHIDS)
		return 0;
	total = READ_ONCE(hold.held_total[chid]);
	since = READ_ONCE(hold.held_since[chid]);
	return total + (since && now > since ? now - since : 0);
}

void tgm_hold_counts(struct tgm_hold_counts *c)
{
	c->holds = READ_ONCE(hold.holds);
	c->preempts = READ_ONCE(hold.preempts);
	c->busy = READ_ONCE(hold.busy);
	c->turned = READ_ONCE(hold.turned_n);
	c->hopeless = READ_ONCE(hold.hopeless_n);
	c->samples = READ_ONCE(hold.samples);
	c->checks = READ_ONCE(hold.checks);
}

uint32_t tgm_hold_turns(unsigned int chid)
{
	return chid < HOLD_CHIDS ? READ_ONCE(hold.turns[chid]) : 0;
}
