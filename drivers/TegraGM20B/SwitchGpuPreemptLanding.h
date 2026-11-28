/* SPDX-License-Identifier: MIT */
/* Debug kernels: where a compositor kick's preempt of an app landed (hold.c). GM20B leaves a graphics context only at a
 * wait for idle, and the driver's banded draws end each band in a wait-for-idle release (draw_bands.h), so a preempt
 * asked mid-job should land at a band's release. A channel with a progress stamp (every app channel asks for one at
 * creation) ends a band in a progress release, which ends no batch; one without, in a seqno release ending a batch. The
 * app's last batch release before the compositor's start stamp, at or after the ask, names a batch landing: inside a
 * job, mid-job; the job's last batch, at the job's end. Failing that, a progress stamp between the ask and the start is
 * a landing mid-job. Neither (the switch came elsewhere, or the release is not read yet): unknown. Each latency also
 * goes in a bucket. Plain C for the host test (test/gm20b_preempt_landing_test.cpp). */
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

static inline enum switch_gpu_landing
switch_gpu_preempt_landing_with_progress(const struct switch_gpu_batch_release *last,
					 uint64_t progress_pt, uint64_t asked_pt, uint64_t start_pt)
{
	const enum switch_gpu_landing landing =
		switch_gpu_preempt_landing(last, asked_pt, start_pt);

	if (landing != SWITCH_GPU_LANDED_UNKNOWN)
		return landing;
	if (progress_pt && progress_pt >= asked_pt && progress_pt <= start_pt)
		return SWITCH_GPU_LANDED_MID_JOB;
	return SWITCH_GPU_LANDED_UNKNOWN;
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
