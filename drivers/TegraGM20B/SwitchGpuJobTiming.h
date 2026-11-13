/* SPDX-License-Identifier: MIT */
/* The stats' job timing (debug kernels): a job's batches (SwitchGpuJobFences.h) each end in a seqno release that stamps
 * the GPU's time, and only the job's first seqno has a start stamp, so a batch's GPU time is its end stamp less the one
 * before it, and the job's is its last end less its start (other channels' time slices inside it included). Each
 * window's "jobs" line gives a process's jobs, its batches by GPU time and its slowest job's batches in order. */
#ifndef SWITCH_GPU_JOB_TIMING_H
#define SWITCH_GPU_JOB_TIMING_H
#include <stdint.h>

#define SWITCH_GPU_JOB_BATCHES 64  /* the slowest job's batches listed */
#define SWITCH_GPU_BATCH_BUCKETS 8 /* under 25 us, then doubling up to 1.6 ms and over */

/* A job whose batches are being read off its stamps, in seqno order */
struct switch_gpu_job_run {
	uint32_t first, last, batches;
	uint64_t start_pt, prev_pt, ns;
	uint32_t us[SWITCH_GPU_JOB_BATCHES];
	int live; /* every stamp so far was this job's */
};

/* A window's jobs. The interrupt pass adds, the window's line takes: counts and sums atomically, the slowest job's list
 * between two generations (odd while it is written). */
struct switch_gpu_job_window {
	uint32_t jobs, batches;
	uint64_t ns, max_ns;
	uint32_t bucket_n[SWITCH_GPU_BATCH_BUCKETS];
	uint64_t bucket_ns[SWITCH_GPU_BATCH_BUCKETS];
	uint32_t slow_gen, slow_first, slow_last, slow_batches;
	uint64_t slow_ns;
	uint32_t slow_us[SWITCH_GPU_JOB_BATCHES];
};

static inline unsigned switch_gpu_batch_bucket(uint64_t ns)
{
	unsigned b = 0;
	for (uint64_t edge = 25000; b + 1 < SWITCH_GPU_BATCH_BUCKETS && ns >= edge; edge *= 2)
		b++;
	return b;
}

static inline void switch_gpu_job_begin(struct switch_gpu_job_run *run, uint32_t first,
					uint32_t last, uint64_t start_pt)
{
	run->first = first;
	run->last = last;
	run->batches = 0;
	run->start_pt = run->prev_pt = start_pt;
	run->ns = 0;
	run->live = 1;
}

/* The batch whose end stamp is end_pt, its GPU time `ns` (the caller converts PTIMER units) */
static inline void switch_gpu_job_batch(struct switch_gpu_job_window *w,
					struct switch_gpu_job_run *run, uint64_t end_pt,
					uint64_t ns)
{
	const unsigned b = switch_gpu_batch_bucket(ns);

	__atomic_fetch_add(&w->bucket_n[b], 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&w->bucket_ns[b], ns, __ATOMIC_RELAXED);
	if (run->batches < SWITCH_GPU_JOB_BATCHES)
		run->us[run->batches] = (uint32_t)(ns / 1000);
	run->batches++;
	run->ns += ns;
	run->prev_pt = end_pt;
}

static inline void switch_gpu_job_end(struct switch_gpu_job_window *w,
				      struct switch_gpu_job_run *run)
{
	uint64_t max = __atomic_load_n(&w->max_ns, __ATOMIC_RELAXED);
	uint32_t n = run->batches < SWITCH_GPU_JOB_BATCHES ? run->batches : SWITCH_GPU_JOB_BATCHES;

	run->live = 0;
	__atomic_fetch_add(&w->jobs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&w->batches, run->batches, __ATOMIC_RELAXED);
	__atomic_fetch_add(&w->ns, run->ns, __ATOMIC_RELAXED);
	while (run->ns > max && !__atomic_compare_exchange_n(&w->max_ns, &max, run->ns, 0,
							     __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
	if (run->ns <= __atomic_load_n(&w->slow_ns, __ATOMIC_RELAXED))
		return;
	__atomic_fetch_add(&w->slow_gen, 1, __ATOMIC_ACQ_REL);
	w->slow_first = run->first;
	w->slow_last = run->last;
	w->slow_batches = run->batches;
	for (uint32_t i = 0; i < n; i++)
		w->slow_us[i] = run->us[i];
	__atomic_store_n(&w->slow_ns, run->ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&w->slow_gen, 1, __ATOMIC_RELEASE);
}

/* Takes the window (zeroing it) into `out`; the slowest job's list is left out (slow_batches 0) if a job was writing
 * it meanwhile. */
static inline void switch_gpu_job_take(struct switch_gpu_job_window *w,
				       struct switch_gpu_job_window *out)
{
	const uint32_t gen = __atomic_load_n(&w->slow_gen, __ATOMIC_ACQUIRE);

	out->jobs = __atomic_exchange_n(&w->jobs, 0, __ATOMIC_RELAXED);
	out->batches = __atomic_exchange_n(&w->batches, 0, __ATOMIC_RELAXED);
	out->ns = __atomic_exchange_n(&w->ns, 0, __ATOMIC_RELAXED);
	out->max_ns = __atomic_exchange_n(&w->max_ns, 0, __ATOMIC_RELAXED);
	for (unsigned b = 0; b < SWITCH_GPU_BATCH_BUCKETS; b++) {
		out->bucket_n[b] = __atomic_exchange_n(&w->bucket_n[b], 0, __ATOMIC_RELAXED);
		out->bucket_ns[b] = __atomic_exchange_n(&w->bucket_ns[b], 0, __ATOMIC_RELAXED);
	}
	out->slow_first = w->slow_first;
	out->slow_last = w->slow_last;
	out->slow_batches = w->slow_batches;
	for (uint32_t i = 0; i < SWITCH_GPU_JOB_BATCHES; i++)
		out->slow_us[i] = w->slow_us[i];
	out->slow_ns = __atomic_exchange_n(&w->slow_ns, 0, __ATOMIC_RELAXED);
	if ((gen & 1) || __atomic_load_n(&w->slow_gen, __ATOMIC_ACQUIRE) != gen)
		out->slow_batches = 0;
}

/* One process's channels' windows, taken, into one */
static inline void switch_gpu_job_merge(struct switch_gpu_job_window *into,
					const struct switch_gpu_job_window *w)
{
	into->jobs += w->jobs;
	into->batches += w->batches;
	into->ns += w->ns;
	if (w->max_ns > into->max_ns)
		into->max_ns = w->max_ns;
	for (unsigned b = 0; b < SWITCH_GPU_BATCH_BUCKETS; b++) {
		into->bucket_n[b] += w->bucket_n[b];
		into->bucket_ns[b] += w->bucket_ns[b];
	}
	if (w->slow_batches && w->slow_ns > into->slow_ns) {
		into->slow_first = w->slow_first;
		into->slow_last = w->slow_last;
		into->slow_batches = w->slow_batches;
		into->slow_ns = w->slow_ns;
		for (uint32_t i = 0; i < SWITCH_GPU_JOB_BATCHES; i++)
			into->slow_us[i] = w->slow_us[i];
	}
}

/* "<owner>: J jobs, B batches (b a job), GPU mean M max X us a job; batches <25us n (s us) ...; slowest F-L: d0 d1 .." */
static inline int switch_gpu_job_format(char *buf, unsigned long len, const char *owner,
					const struct switch_gpu_job_window *w)
{
	static const char *const edge[SWITCH_GPU_BATCH_BUCKETS] = {
		"<25", "<50", "<100", "<200", "<400", "<800", "<1600", ">=1600"};
	unsigned long at = 0;
	uint32_t shown =
		w->slow_batches < SWITCH_GPU_JOB_BATCHES ? w->slow_batches : SWITCH_GPU_JOB_BATCHES;

	if (!w->jobs)
		return 0;
#define SWITCH_JOB_PUT(...)                                                  \
	do {                                                                 \
		if (at < len) {                                              \
			int put = snprintf(buf + at, len - at, __VA_ARGS__); \
			at += put > 0 ? (unsigned long)put : 0;              \
		}                                                            \
	} while (0)
	SWITCH_JOB_PUT("%s: %u jobs, %u batches (%u.%u a job), GPU mean %llu max %llu us a job; "
		       "batches by GPU time:",
		       owner, w->jobs, w->batches, w->batches / w->jobs,
		       (w->batches * 10 / w->jobs) % 10,
		       (unsigned long long)(w->ns / w->jobs / 1000),
		       (unsigned long long)(w->max_ns / 1000));
	for (unsigned b = 0; b < SWITCH_GPU_BATCH_BUCKETS; b++)
		if (w->bucket_n[b])
			SWITCH_JOB_PUT(" %sus %u (%llu us)", edge[b], w->bucket_n[b],
				       (unsigned long long)(w->bucket_ns[b] / 1000));
	if (shown) {
		SWITCH_JOB_PUT("; slowest seqno %u-%u, %llu us:", w->slow_first, w->slow_last,
			       (unsigned long long)(w->slow_ns / 1000));
		for (uint32_t i = 0; i < shown; i++)
			SWITCH_JOB_PUT(" %u", w->slow_us[i]);
		if (w->slow_batches > shown)
			SWITCH_JOB_PUT(" +%u", w->slow_batches - shown);
	}
#undef SWITCH_JOB_PUT
	if (at >= len && len)
		buf[len - 1] = 0;
	return 1;
}

#endif
