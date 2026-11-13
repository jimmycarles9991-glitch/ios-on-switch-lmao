/* SPDX-License-Identifier: MIT */
/* A job's internal fences (TegraGM20BPushFence): one submission carries a command buffer's work batches, and the GPU
 * releases the channel's next seqno at the end of each batch, as when each batch was its own submission. The job's
 * resources are resolved and owned once, against the seqno of its own fence; the internal ones keep the channel's
 * completed seqno moving while the job runs, silently: only the job's own fence interrupts, and the stuck rule of
 * TegraGM20BRecover.h reads the fence word when it checks. */
#pragma once
#include <stdint.h>
#include "TegraGM20BABI.h"
#include "TegraGM20BGpu.h"

/* An internal fence's kernel words, with room to spare: an end stamp, the WFI semaphore release and the GET report of
 * the entry that carries them (5 + 5 + 5). */
#define SWITCH_GPU_FENCE_POINT_DWORDS 32

/* A channel's last jobs by the seqnos each released (`first` .. `last`), with what its kick found (high channels,
 * gpu_fenced.h high_fenced). The interrupt pass covers whole jobs and a fence waiter's seqno is announced by the end of its job,
 * so both find a seqno's job here. A record being refilled is empty (`first` past `last`) until `last` is published. A
 * quiet job's end announces nothing until a wake entry follows it (SwitchGpuQuiet.h). */
#define SWITCH_GPU_JOBS 8

struct switch_gpu_kicked_job {
	uint64_t kick_ns, asked_pt;
	uint32_t first, last;
	int16_t behind;
	uint8_t app, waits, quiet;
};

struct switch_gpu_jobs {
	struct switch_gpu_kicked_job job[SWITCH_GPU_JOBS];
	uint32_t kicked;
};

static inline void switch_gpu_jobs_init(struct switch_gpu_jobs *jobs, uint32_t submitted)
{
	for (uint32_t i = 0; i < SWITCH_GPU_JOBS; ++i) {
		jobs->job[i].first = submitted + 1;
		jobs->job[i].last = submitted;
		jobs->job[i].behind = -1;
		jobs->job[i].quiet = 0;
	}
}

/* The record the channel's next kick fills. */
static inline struct switch_gpu_kicked_job *switch_gpu_jobs_next(struct switch_gpu_jobs *jobs)
{
	return &jobs->job[jobs->kicked % SWITCH_GPU_JOBS];
}

/* Before the kick: the job releasing first .. last, as kicked behind an app until the kick's result says otherwise. */
static inline struct switch_gpu_kicked_job *switch_gpu_jobs_kick(struct switch_gpu_jobs *jobs,
								 uint32_t first, uint32_t last,
								 uint64_t kick_ns, int quiet)
{
	struct switch_gpu_kicked_job *job = switch_gpu_jobs_next(jobs);

	__atomic_store_n(&job->first, last + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&job->kick_ns, kick_ns, __ATOMIC_RELAXED);
	__atomic_store_n(&job->behind, -1, __ATOMIC_RELAXED);
	__atomic_store_n(&job->app, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&job->quiet, quiet ? 1 : 0, __ATOMIC_RELAXED);
	__atomic_store_n(&job->first, first, __ATOMIC_RELEASE);
	__atomic_store_n(&job->last, last, __ATOMIC_RELEASE);
	jobs->kicked++;
	return job;
}

/* A copy of the record holding `seqno`; 0 if none does (never kicked here, or refilled since, the copy included). */
static inline int switch_gpu_jobs_find(const struct switch_gpu_jobs *jobs, uint32_t seqno,
				       struct switch_gpu_kicked_job *out)
{
	for (uint32_t i = 0; i < SWITCH_GPU_JOBS; ++i) {
		const struct switch_gpu_kicked_job *job = &jobs->job[i];
		const uint32_t last = __atomic_load_n(&job->last, __ATOMIC_ACQUIRE);
		const uint32_t first = __atomic_load_n(&job->first, __ATOMIC_ACQUIRE);

		if ((int32_t)(seqno - first) < 0 || (int32_t)(last - seqno) < 0)
			continue;
		out->kick_ns = __atomic_load_n(&job->kick_ns, __ATOMIC_RELAXED);
		out->asked_pt = __atomic_load_n(&job->asked_pt, __ATOMIC_RELAXED);
		out->behind = __atomic_load_n(&job->behind, __ATOMIC_RELAXED);
		out->app = __atomic_load_n(&job->app, __ATOMIC_RELAXED);
		out->waits = __atomic_load_n(&job->waits, __ATOMIC_RELAXED);
		out->quiet = __atomic_load_n(&job->quiet, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&job->first, __ATOMIC_RELAXED) != first)
			continue;
		out->first = first;
		out->last = last;
		return 1;
	}
	return 0;
}

/* The records holding every seqno of first .. last, in order; 0 if one of them is gone. */
static inline uint32_t switch_gpu_jobs_span(const struct switch_gpu_jobs *jobs, uint32_t first,
					    uint32_t last,
					    struct switch_gpu_kicked_job out[SWITCH_GPU_JOBS])
{
	uint32_t n = 0;

	for (uint32_t seqno = first; (int32_t)(last - seqno) >= 0; seqno = out[n - 1].last + 1) {
		if (n == SWITCH_GPU_JOBS || !switch_gpu_jobs_find(jobs, seqno, &out[n]))
			return 0;
		n++;
	}
	return n;
}

/* The seqno whose fence interrupts for `seqno`: the end of the recorded job holding it, else of a later recorded job
 * (which ends after it too), else `seqno` itself (not kicked yet). A quiet job's end is passed over: *announced is 0 when
 * every recorded end at or after `seqno` is quiet. */
static inline uint32_t switch_gpu_jobs_announced_by(const struct switch_gpu_jobs *jobs,
						    uint32_t seqno, int *announced)
{
	uint32_t best = seqno;
	int found = 0, kicked = 0;

	for (uint32_t i = 0; i < SWITCH_GPU_JOBS; ++i) {
		const uint32_t last = __atomic_load_n(&jobs->job[i].last, __ATOMIC_ACQUIRE);
		const uint32_t first = __atomic_load_n(&jobs->job[i].first, __ATOMIC_ACQUIRE);

		if ((int32_t)(last - first) < 0 || (int32_t)(last - seqno) < 0)
			continue;
		kicked = 1;
		if (__atomic_load_n(&jobs->job[i].quiet, __ATOMIC_ACQUIRE))
			continue;
		if (!found || (int32_t)(last - best) < 0)
			best = last;
		found = 1;
	}
	*announced = found || !kicked;
	return best;
}

static inline uint32_t switch_gpu_jobs_announcing(const struct switch_gpu_jobs *jobs,
						  uint32_t seqno)
{
	int announced = 0;

	return switch_gpu_jobs_announced_by(jobs, seqno, &announced);
}

/* A wake entry pushed behind the newest job (the submission's owner): its end announces from now on. */
static inline void switch_gpu_jobs_announce_newest(struct switch_gpu_jobs *jobs)
{
	struct switch_gpu_kicked_job *job = &jobs->job[(jobs->kicked - 1) % SWITCH_GPU_JOBS];

	__atomic_store_n(&job->quiet, 0, __ATOMIC_RELEASE);
}

/* 0 and the count, or -22 for a fence after an incomplete push or the job's last, -7 for more than the ABI allows. */
static inline int switch_gpu_job_fences(const struct tgm_push *pushes, uint32_t count,
					uint32_t *fences)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (!pushes[i].fence)
			continue;
		if (i + 1 == count || pushes[i].incomplete)
			return -22;
		++n;
	}
	if (n > TegraGM20BMaxJobFences)
		return -7;
	*fences = n;
	return 0;
}

struct switch_gpu_job_emitter {
	void *context;
	void (*entry)(void *context, uint32_t push);
	void (*fence)(void *context, uint32_t seqno);
};

/* The user entries in order, an internal fence releasing the next seqno after each marked push. Returns the seqno
 * the job's own fence releases: `submitted` + the internal fences + 1. */
static inline uint32_t switch_gpu_job_emit(const struct tgm_push *pushes, uint32_t count,
					   uint32_t submitted,
					   const struct switch_gpu_job_emitter *emitter)
{
	uint32_t seqno = submitted + 1;
	for (uint32_t i = 0; i < count; ++i) {
		emitter->entry(emitter->context, i);
		if (pushes[i].fence)
			emitter->fence(emitter->context, seqno++);
	}
	return seqno;
}
