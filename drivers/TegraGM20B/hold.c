// SPDX-License-Identifier: MIT
/*
 * The hold (TegraGM20BSched.h): app channels kept off the GPU for the compositor's frame, each from its own preemption
 * latency before the compositor's latest start (deadline.c picks the times) until the present completes. A hold is
 * nvkm's own counted block (chan->blocked under chan->lock: gk104_chan_stop's ENABLE_CLR, gk104_chan_start's ENABLE_SET
 * once the count is back at zero), so it composes with nvkm's blocks for a teardown or a context bind; the runlist is
 * never rebuilt for it (nv50_runl_wait polls). The channel the GPU is on is preempted too, and leaves at its next
 * wait-for-idle: its preemption latency is learned from the compositor's start stamp after it (gpu_fenced.h), and brought down
 * when a check half that latency after a preempt finds it gone.
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
#include "SwitchGpuPreemptLanding.h"

#define HOLD_CHIDS 512 /* GM20B's channels (CCSR_CHANNEL__SIZE_1) */
#define HOLD_LATENCY_MAX TGM_INTERVAL_MAX_NS

static struct {
	spinlock_t lock;
	DECLARE_BITMAP(tracked, HOLD_CHIDS); /* gpu_chan.c's app channels */
	DECLARE_BITMAP(needed, HOLD_CHIDS);  /* high work waits for them, since need_ns */
	DECLARE_BITMAP(held, HOLD_CHIDS);
	DECLARE_BITMAP(held_last, HOLD_CHIDS); /* held in the last window, not yet given its turn */
	DECLARE_BITMAP(turned, HOLD_CHIDS);    /* given its turn this window */
	DECLARE_BITMAP(hopeless,
		       HOLD_CHIDS); /* stats: its hold put off to the next vblank this window */
	DECLARE_BITMAP(isolating, HOLD_CHIDS); /* gpu_hang.c is killing it (tgm_hold_drop) */
	DECLARE_BITMAP(slow, HOLD_CHIDS); /* tracked, its latency worth a hold (tgm_hold_worth) */
	u32 latency[HOLD_CHIDS];          /* preemption latency, ns */
	u64 asked_pt[HOLD_CHIDS],
		asked_ns[HOLD_CHIDS]; /* our ask to leave the GPU not yet sampled: PTIMER, kernel clock */
	u64 held_since[HOLD_CHIDS], held_total[HOLD_CHIDS], need_ns[HOLD_CHIDS];
	u64 closed; /* when the last window released its holds */
	u16 turns[HOLD_CHIDS];
	u32 holds, preempts, busy, turned_n, hopeless_n, samples, checks;
} hold;

#if TGM_STATS
extern int proc_selfpid(void);

/* Each tracked channel's process and the preemption latencies sampled for it since that process last read them
 * (tgm_hold_samples_take) */
static struct {
	int pid[HOLD_CHIDS];
	u32 n[HOLD_CHIDS];
	u64 max_ns[HOLD_CHIDS];
	struct switch_gpu_batch_release
		last[HOLD_CHIDS]; /* each app channel's last batch release (hold.lock) */
	struct switch_gpu_preempt_counts counts[HOLD_CHIDS];
} sampled;
#endif

void tgm_hold_init(void)
{
	static int once;
	unsigned int i = 0;

	if (once)
		return;
	once = 1;
	spin_lock_init(&hold.lock);
	for (i = 0; i < HOLD_CHIDS; i++)
		hold.latency[i] = TGM_PREEMPT_MIN_NS;
}

static struct nvkm_chan *chan_get(struct nvkm_device *device, unsigned int chid,
				  unsigned long *flags)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *chan = NULL;

	nvkm_runl_foreach(runl, device->fifo)
	{
		if ((int)chid >= runl->chid->nr)
			continue;
		chan = nvkm_runl_chan_get_chid(runl, chid, flags);
		if (chan)
			return chan;
	}
	return NULL;
}

/* hold.lock held */
static void latency_set(unsigned int chid, u64 ns)
{
	hold.latency[chid] = (u32)ns;
	if (tgm_hold_worth(ns) && test_bit(chid, hold.tracked)) {
		__set_bit(chid, hold.slow);
	} else {
		__clear_bit(chid, hold.slow);
	}
}

/* hold.lock held */
static void held_end(unsigned int chid, u64 now)
{
	if (!test_and_clear_bit(chid, hold.held))
		return;
	WRITE_ONCE(hold.held_total[chid],
		   hold.held_total[chid] +
			   (now > hold.held_since[chid] ? now - hold.held_since[chid] : 0));
	WRITE_ONCE(hold.held_since[chid], 0);
}

/* hold.lock held */
static int chan_hold(struct nvkm_device *device, unsigned int chid, u64 now)
{
	unsigned long flags = 0;
	unsigned long irq = 0;
	struct nvkm_chan *chan = chan_get(device, chid, &flags);
	int held = 0;

	if (!chan)
		return 0;
	spin_lock_irqsave(&chan->lock, irq);
	if (tgm_may_hold(TGM_LEVEL_FOREGROUND, (int)test_bit(chid, hold.tracked),
			 (int)test_bit(chid, hold.needed), atomic_read(&chan->errored),
			 (int)test_bit(chid, hold.isolating), atomic_read(&chan->cgrp->rc),
			 atomic_read(&chan->blocked))) {
		atomic_inc(&chan->blocked);
		chan->func->stop(chan);
		held = 1;
	}
	spin_unlock_irqrestore(&chan->lock, irq);
	nvkm_chan_put(&chan, flags);
	if (held) {
		__set_bit(chid, hold.held);
		WRITE_ONCE(hold.held_since[chid], now ? now : 1);
		if (TGM_STATS)
			hold.holds++;
	}
	return held;
}

/* hold.lock held. A channel errored or put into recovery since it was held stays blocked: nvkm's recovery owns it. */
static void chan_release(struct nvkm_device *device, unsigned int chid, u64 now)
{
	unsigned long flags = 0;
	unsigned long irq = 0;
	struct nvkm_chan *chan = NULL;

	if (!test_bit(chid, hold.held))
		return;
	held_end(chid, now);
	chan = chan_get(device, chid, &flags);
	if (!chan)
		return;
	spin_lock_irqsave(&chan->lock, irq);
	if (tgm_may_release(atomic_read(&chan->errored), (int)test_bit(chid, hold.isolating),
			    atomic_read(&chan->cgrp->rc)) &&
	    atomic_dec_and_test(&chan->blocked))
		chan->func->start(chan);
	spin_unlock_irqrestore(&chan->lock, irq);
	nvkm_chan_put(&chan, flags);
}

void tgm_hold_track(struct nvkm_device *device, unsigned int chid, int on)
{
	unsigned long flags = 0;

	if (chid >= HOLD_CHIDS)
		return;
	spin_lock_irqsave(&hold.lock, flags);
	if (on) {
#if TGM_STATS
		WRITE_ONCE(sampled.pid[chid], proc_selfpid());
#endif
		__set_bit(chid, hold.tracked);
		__clear_bit(chid, hold.isolating);
		latency_set(chid, TGM_PREEMPT_MIN_NS);
		hold.asked_pt[chid] = 0;
		__clear_bit(chid, hold.needed);
		__clear_bit(chid, hold.held_last);
		__clear_bit(chid, hold.turned);
	} else {
		chan_release(device, chid, nvxnu_nanotime());
		__clear_bit(chid, hold.tracked);
		__clear_bit(chid, hold.slow);
	}
	spin_unlock_irqrestore(&hold.lock, flags);
}

void tgm_hold_drop(unsigned int chid)
{
	unsigned long flags = 0;

	if (chid >= HOLD_CHIDS)
		return;
	spin_lock_irqsave(&hold.lock, flags);
	held_end(chid, nvxnu_nanotime());
	__set_bit(chid, hold.isolating);
	__clear_bit(chid, hold.tracked);
	spin_unlock_irqrestore(&hold.lock, flags);
}

void tgm_hold_need(struct nvkm_device *device, unsigned int chid)
{
	unsigned long flags = 0;

	if (chid >= HOLD_CHIDS)
		return;
	spin_lock_irqsave(&hold.lock, flags);
	__set_bit(chid, hold.needed);
	hold.need_ns[chid] = nvxnu_nanotime();
	chan_release(device, chid, hold.need_ns[chid]);
	spin_unlock_irqrestore(&hold.lock, flags);
}

int tgm_hold_needed(unsigned int chid)
{
	return chid < HOLD_CHIDS && test_bit(chid, hold.needed);
}

void tgm_hold_unneed(uint64_t queued)
{
	unsigned long flags = 0;
	unsigned long chid = 0;

	spin_lock_irqsave(&hold.lock, flags);
	for_each_set_bit(chid, hold.needed, HOLD_CHIDS)
	{
		if (hold.need_ns[chid] <= queued)
			__clear_bit(chid, hold.needed);
	}
	spin_unlock_irqrestore(&hold.lock, flags);
}

uint64_t tgm_hold_due(struct nvkm_device *device, const struct tgm_window *w, int resident,
		      int *preempt)
{
	unsigned long flags = 0;
	unsigned long chid = 0;
	u64 next = 0;
	u64 at = 0;
	u64 latency = 0;

	*preempt = -1;
	spin_lock_irqsave(&hold.lock, flags);
	for_each_set_bit(chid, hold.slow, HOLD_CHIDS)
	{
		if (test_bit(chid, hold.held) || test_bit(chid, hold.turned) ||
		    test_bit(chid, hold.needed))
			continue;
		latency = hold.latency[chid];
		if (tgm_owed_turn((int)test_bit(chid, hold.held_last), w->now - hold.closed,
				  latency)) {
			__set_bit(chid, hold.turned);
			__clear_bit(chid, hold.held_last);
			if (TGM_STATS) {
				hold.turned_n++;
				hold.turns[chid]++;
			}
			continue;
		}
		at = tgm_hold_time(w->vblank, w->interval, w->now, w->expected, latency,
				   (int)chid == resident);
		if (at > w->now) {
			if (TGM_STATS && at > w->vblank && !test_and_set_bit(chid, hold.hopeless))
				hold.hopeless_n++;
			next = !next || at < next ? at : next;
			continue;
		}
		if (chan_hold(device, chid, w->now) && (int)chid == resident)
			*preempt = (int)chid;
	}
	spin_unlock_irqrestore(&hold.lock, flags);
	return next;
}

int tgm_hold_preempt(struct nvkm_device *device, unsigned int chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *chan = NULL;
	unsigned long flags = 0;
	u64 pt = 0;

	if (chid >= HOLD_CHIDS)
		return 0;
	chan = chan_get(device, chid, &flags);
	if (chan) {
		runl = chan->cgrp->runl;
		nvkm_chan_put(&chan, flags);
	}
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
	spin_lock_irqsave(&hold.lock, flags);
	/* a kick's own preempt asked first: the latency runs from that ask */
	if (!hold.asked_pt[chid]) {
		hold.asked_pt[chid] = pt;
		hold.asked_ns[chid] = nvxnu_nanotime();
	}
	if (TGM_STATS)
		hold.preempts++;
	spin_unlock_irqrestore(&hold.lock, flags);
	return 1;
busy:
	if (TGM_STATS)
		__atomic_fetch_add(&hold.busy, 1, __ATOMIC_RELAXED);
	return 0;
}

uint64_t tgm_hold_asked(unsigned int chid, uint64_t asked_pt)
{
	unsigned long flags = 0;
	u64 asked = 0;

	if (chid >= HOLD_CHIDS)
		return 0;
	spin_lock_irqsave(&hold.lock, flags);
	if (!hold.asked_pt[chid] && asked_pt) {
		hold.asked_pt[chid] = asked_pt;
		hold.asked_ns[chid] = nvxnu_nanotime();
	}
	asked = hold.asked_pt[chid];
	spin_unlock_irqrestore(&hold.lock, flags);
	return asked;
}

void tgm_hold_sampled(unsigned int chid, uint64_t asked_pt, uint64_t start_pt)
{
	unsigned long flags = 0;
	u64 sample = 0;

	if (chid >= HOLD_CHIDS)
		return;
	spin_lock_irqsave(&hold.lock, flags);
	/* only the first submission behind an ask: later ones also waited for the compositor's own work before them */
	if (hold.asked_pt[chid] == asked_pt && start_pt > asked_pt)
		sample = tgm_deadline_pt_ns(start_pt - asked_pt);
	if (sample) {
		latency_set(chid,
			    tgm_preempt_sampled(hold.latency[chid], sample, HOLD_LATENCY_MAX));
		hold.asked_pt[chid] = 0;
		if (TGM_STATS)
			hold.samples++;
#if TGM_STATS
		__atomic_fetch_add(&sampled.n[chid], 1, __ATOMIC_RELAXED);
		if (sample > __atomic_load_n(&sampled.max_ns[chid], __ATOMIC_RELAXED))
			__atomic_store_n(&sampled.max_ns[chid], sample, __ATOMIC_RELAXED);
		sampled.counts[chid].landed[switch_gpu_preempt_landing(&sampled.last[chid],
								       asked_pt, start_pt)]++;
		sampled.counts[chid].bucket[switch_gpu_preempt_bucket(sample)]++;
#endif
	}
	spin_unlock_irqrestore(&hold.lock, flags);
}

#if TGM_STATS
void tgm_hold_samples_take(int pid, uint32_t *n, uint64_t *max_ns,
			   struct switch_gpu_preempt_counts *counts)
{
	unsigned long flags = 0;
	unsigned int i = 0;
	u64 max = 0;

	*n = 0;
	memset(counts, 0, sizeof(*counts));
	for (i = 0; i < HOLD_CHIDS; i++) {
		if (READ_ONCE(sampled.pid[i]) != pid)
			continue;
		*n += __atomic_exchange_n(&sampled.n[i], 0, __ATOMIC_RELAXED);
		max = max_t(u64, max, __atomic_exchange_n(&sampled.max_ns[i], 0, __ATOMIC_RELAXED));
		spin_lock_irqsave(&hold.lock, flags);
		switch_gpu_preempt_add(counts, &sampled.counts[i]);
		memset(&sampled.counts[i], 0, sizeof(sampled.counts[i]));
		spin_unlock_irqrestore(&hold.lock, flags);
	}
	*max_ns = max;
}
#endif

void tgm_hold_released(unsigned int chid, uint64_t end_pt, int job_last)
{
#if TGM_STATS
	unsigned long flags = 0;

	if (chid >= HOLD_CHIDS)
		return;
	spin_lock_irqsave(&hold.lock, flags);
	sampled.last[chid].end_pt = end_pt;
	sampled.last[chid].job_last = job_last != 0;
	spin_unlock_irqrestore(&hold.lock, flags);
#else
	(void)chid;
	(void)end_pt;
	(void)job_last;
#endif
}

void tgm_hold_check(struct nvkm_device *device, unsigned int chid)
{
	int resident = tgm_sched_resident(device);
	unsigned long flags = 0;

	if (chid >= HOLD_CHIDS || resident == (int)chid || resident == TGM_VICTIM_SWITCHING)
		return;
	spin_lock_irqsave(&hold.lock, flags);
	if (hold.asked_pt[chid]) {
		latency_set(chid, tgm_preempt_left_early(hold.latency[chid]));
		hold.asked_pt[chid] = 0;
		if (TGM_STATS)
			hold.checks++;
	}
	spin_unlock_irqrestore(&hold.lock, flags);
}

int tgm_hold_wanted(void)
{
	return !bitmap_empty(hold.slow, HOLD_CHIDS);
}

uint64_t tgm_hold_latency(unsigned int chid)
{
	return chid < HOLD_CHIDS ? READ_ONCE(hold.latency[chid]) : TGM_PREEMPT_MIN_NS;
}

void tgm_hold_release(struct nvkm_device *device, uint64_t now)
{
	unsigned long flags = 0;
	unsigned long chid = 0;

	spin_lock_irqsave(&hold.lock, flags);
	bitmap_copy(hold.held_last, hold.held, HOLD_CHIDS);
	for_each_set_bit(chid, hold.held_last, HOLD_CHIDS) chan_release(device, chid, now);
	bitmap_zero(hold.turned, HOLD_CHIDS);
	bitmap_zero(hold.hopeless, HOLD_CHIDS);
	/* an ask no submission queued behind (the channel left before the compositor came): no sample is coming for it */
	for (chid = 0; chid < HOLD_CHIDS; chid++) {
		if (hold.asked_pt[chid] && now - hold.asked_ns[chid] > TGM_INTERVAL_MAX_NS)
			hold.asked_pt[chid] = 0;
	}
	hold.closed = now;
	spin_unlock_irqrestore(&hold.lock, flags);
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

void tgm_hold_chan_stats(unsigned int chid, uint64_t *latency, unsigned int *turns)
{
	*latency = tgm_hold_latency(chid);
	*turns =
		chid < HOLD_CHIDS ? __atomic_exchange_n(&hold.turns[chid], 0, __ATOMIC_RELAXED) : 0;
}

void tgm_hold_stats(char *buf, unsigned int len)
{
	unsigned long flags = 0;
	u32 holds = 0;
	u32 preempts = 0;
	u32 busy = 0;
	u32 turned = 0;
	u32 hopeless = 0;
	u32 samples = 0;
	u32 checks = 0;

	spin_lock_irqsave(&hold.lock, flags);
	holds = hold.holds;
	preempts = hold.preempts;
	turned = hold.turned_n;
	hopeless = hold.hopeless_n;
	samples = hold.samples;
	checks = hold.checks;
	hold.holds = hold.preempts = hold.turned_n = hold.hopeless_n = hold.samples = hold.checks =
		0;
	spin_unlock_irqrestore(&hold.lock, flags);
	busy = __atomic_exchange_n(&hold.busy, 0, __ATOMIC_RELAXED);
	scnprintf(buf, len,
		  "%u app channels held, %u early preempts (%u skipped: a preempt in flight or the "
		  "runlist busy), %u one-period turns, %u holds put off to the next vblank "
		  "(hopeless), "
		  "preemption latency %u samples, %u lowered",
		  holds, preempts, busy, turned, hopeless, samples, checks);
}
