// SPDX-License-Identifier: MIT
// SubmitJob and resource sets (debug kernels): each stats window's SubmitJobs by phase, how often they named a set and
// found it current, their shared uses and sibling acquires, and what ended set generations, every process together: the
// periodic "jobs" line. A release kernel has the calls only, empty.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define SWITCH_GPU_JOB_STATS 1
/* ns: TegraGM20BGpu.h's TGM_SUBMIT_* phases */
void switch_gpu_job_submitted(const uint64_t *ns, int named_set, uint32_t shared_uses,
			      uint32_t siblings);
void switch_gpu_job_stale(void);
void switch_gpu_job_invalidated(uint32_t cause);
/* The window's counts as "SubmitJob N x mean ...", zeroed for the next window; empty when nothing happened */
void switch_gpu_job_window(char *line, size_t size);
#else
#define SWITCH_GPU_JOB_STATS 0

static inline void switch_gpu_job_submitted(const uint64_t *ns, int named_set, uint32_t shared_uses,
					    uint32_t siblings)
{
	(void)ns;
	(void)named_set;
	(void)shared_uses;
	(void)siblings;
}

static inline void switch_gpu_job_stale(void)
{
}

static inline void switch_gpu_job_invalidated(uint32_t cause)
{
	(void)cause;
}

static inline void switch_gpu_job_window(char *line, size_t size)
{
	if (size)
		line[0] = 0;
}
#endif

#ifdef __cplusplus
}
#endif
