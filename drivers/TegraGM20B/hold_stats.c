// SPDX-License-Identifier: MIT
/* The hold's stats lines, and in debug kernels each tracked channel's process and the preemption latencies sampled for
 * it since that process last read them (tgm_hold_samples_take). The lines have one caller at a time (the window's
 * winner, tgm_stats_tick). */
#include <core/device.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BSched.h"
#include "SwitchGpuPreemptLanding.h"
#include "deadline_stats.h"

#define HOLD_CHIDS 512 /* GM20B's channels (CCSR_CHANNEL__SIZE_1) */

#if TGM_STATS
extern int proc_selfpid(void);

/* The gate writes the landings; `lock` keeps them whole against tgm_hold_samples_take, and is taken in the gate only
 * when a sample lands. */
static struct {
	spinlock_t lock;
	int pid[HOLD_CHIDS];
	u32 n[HOLD_CHIDS];
	u64 max_ns[HOLD_CHIDS];
	struct switch_gpu_batch_release
		last[HOLD_CHIDS]; /* each app channel's last batch release (the gate's) */
	struct switch_gpu_preempt_counts counts[HOLD_CHIDS];
} sampled;
#endif

void tgm_hold_owner(unsigned int chid)
{
#if TGM_STATS
	if (chid < HOLD_CHIDS)
		WRITE_ONCE(sampled.pid[chid], proc_selfpid());
#else
	(void)chid;
#endif
}

void tgm_hold_landed(unsigned int chid, uint64_t asked_pt, uint64_t start_pt, uint64_t sample)
{
#if TGM_STATS
	unsigned long flags = 0;

	if (chid >= HOLD_CHIDS)
		return;
	__atomic_fetch_add(&sampled.n[chid], 1, __ATOMIC_RELAXED);
	if (sample > __atomic_load_n(&sampled.max_ns[chid], __ATOMIC_RELAXED))
		__atomic_store_n(&sampled.max_ns[chid], sample, __ATOMIC_RELAXED);
	spin_lock_irqsave(&sampled.lock, flags);
	sampled.counts[chid]
		.landed[switch_gpu_preempt_landing(&sampled.last[chid], asked_pt, start_pt)]++;
	sampled.counts[chid].bucket[switch_gpu_preempt_bucket(sample)]++;
	spin_unlock_irqrestore(&sampled.lock, flags);
#else
	(void)chid;
	(void)asked_pt;
	(void)start_pt;
	(void)sample;
#endif
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
		spin_lock_irqsave(&sampled.lock, flags);
		switch_gpu_preempt_add(counts, &sampled.counts[i]);
		memset(&sampled.counts[i], 0, sizeof(sampled.counts[i]));
		spin_unlock_irqrestore(&sampled.lock, flags);
	}
	*max_ns = max;
}
#endif

void tgm_hold_released(unsigned int chid, uint64_t end_pt, int job_last)
{
#if TGM_STATS
	if (chid >= HOLD_CHIDS)
		return;
	sampled.last[chid].end_pt = end_pt;
	sampled.last[chid].job_last = job_last != 0;
#else
	(void)chid;
	(void)end_pt;
	(void)job_last;
#endif
}

static struct {
	struct tgm_hold_counts counts;
	u32 turns[HOLD_CHIDS];
} seen;

void tgm_hold_chan_stats(unsigned int chid, uint64_t *latency, unsigned int *turns)
{
	const u32 now = tgm_hold_turns(chid);

	*latency = tgm_hold_latency(chid);
	*turns = 0;
	if (chid < HOLD_CHIDS) {
		*turns = now - seen.turns[chid];
		seen.turns[chid] = now;
	}
}

void tgm_hold_stats(char *buf, unsigned int len)
{
	struct tgm_hold_counts now;
	const struct tgm_hold_counts *was = &seen.counts;

	tgm_hold_counts(&now);
	scnprintf(buf, len,
		  "%u app channels held, %u early preempts (%u skipped: a preempt in flight or the "
		  "runlist busy), %u one-period turns, %u holds put off to the next vblank "
		  "(hopeless), "
		  "preemption latency %u samples, %u lowered",
		  now.holds - was->holds, now.preempts - was->preempts, now.busy - was->busy,
		  now.turned - was->turned, now.hopeless - was->hopeless,
		  now.samples - was->samples, now.checks - was->checks);
	seen.counts = now;
}
