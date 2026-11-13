/* SPDX-License-Identifier: MIT */
/* Debug kernels: where a compositor kick's preempt of an app landed (hold.c). GM20B leaves a graphics context only at a
 * wait for idle, and the raw path's banded draws end each band in a seqno release (raw_draw_bands.h), so a preempt asked
 * mid-job should land at a band's release: the app's last release seen before the compositor's start stamp, at or after
 * the ask, names it. That release ending a batch inside a job is a landing mid-job; ending the job's last batch, at the
 * job's end; none between the ask and the start (the switch came elsewhere, or the release is not read yet), unknown.
 * Each latency also goes in a bucket. Plain C for the host test (test/gm20b_preempt_landing_test.cpp). */
#ifndef SWITCH_GPU_PREEMPT_LANDING_H
#define SWITCH_GPU_PREEMPT_LANDING_H
#include <stdint.h>

#define SWITCH_GPU_PREEMPT_BUCKETS 6 /* under 250 us, then doubling up to 4 ms and over */

/* An app channel's last batch release: its end stamp (PTIMER), and whether it ended the job */
struct switch_gpu_batch_release {
	uint64_t end_pt;
	uint8_t job_last;
};

enum switch_gpu_landing {
	SWITCH_GPU_LANDED_UNKNOWN,
	SWITCH_GPU_LANDED_MID_JOB,
	SWITCH_GPU_LANDED_JOB_END,
};

/* A process's landings and latency buckets since its last read */
struct switch_gpu_preempt_counts {
	uint32_t landed[3];
	uint32_t bucket[SWITCH_GPU_PREEMPT_BUCKETS];
};

static inline enum switch_gpu_landing
switch_gpu_preempt_landing(const struct switch_gpu_batch_release *last, uint64_t asked_pt,
			   uint64_t start_pt)
{
	if (!last->end_pt || last->end_pt < asked_pt || last->end_pt > start_pt)
		return SWITCH_GPU_LANDED_UNKNOWN;
	return last->job_last ? SWITCH_GPU_LANDED_JOB_END : SWITCH_GPU_LANDED_MID_JOB;
}

static inline unsigned switch_gpu_preempt_bucket(uint64_t ns)
{
	unsigned b = 0;

	for (uint64_t edge = 250000; b + 1 < SWITCH_GPU_PREEMPT_BUCKETS && ns >= edge; edge *= 2)
		b++;
	return b;
}

static inline void switch_gpu_preempt_add(struct switch_gpu_preempt_counts *into,
					  const struct switch_gpu_preempt_counts *from)
{
	for (unsigned i = 0; i < 3; i++)
		into->landed[i] += from->landed[i];
	for (unsigned i = 0; i < SWITCH_GPU_PREEMPT_BUCKETS; i++)
		into->bucket[i] += from->bucket[i];
}

#endif
