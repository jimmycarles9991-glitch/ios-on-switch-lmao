/* SPDX-License-Identifier: MIT */
/* Debug kernels: what the compositor did between two asks of the spans op (TegraGM20BPathsABI.h), for a HW Test that runs
 * one animation between them (ui.scale_pacing): its jobs, their GPU time and those longer than a refresh period, the
 * surfaces it imported and their time, its BoAllocs (cold: made on its own thread, no zeroed spare), and in its
 * push-scanned submissions the render passes, draws, interlock enables and barrier twin draws
 * (TegraGM20BPushScan.h); and the deadline windows opened and app channels held meanwhile. Plain C for the
 * host test (test/gm20b_compositor_round_test.cpp). */
#ifndef SWITCH_GPU_COMPOSITOR_ROUND_H
#define SWITCH_GPU_COMPOSITOR_ROUND_H
#include <stdint.h>
#include <string.h>

struct switch_gpu_comp_round {
	uint64_t jobs, gpu_ns, over_period, imports, import_ns, allocs, alloc_ns, cold_allocs,
		scanned, passes, draws, interlocks, twins, windows, holds;
};

/* The deadline's and the hold's counts (deadline_stats.h), which only grow, as the round before read them */
struct switch_gpu_comp_deadline {
	uint32_t windows, holds;
};

static inline void switch_gpu_comp_round_job(struct switch_gpu_comp_round *r, uint64_t ns,
					     uint64_t period_ns)
{
	__atomic_fetch_add(&r->jobs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&r->gpu_ns, ns, __ATOMIC_RELAXED);
	if (ns > period_ns)
		__atomic_fetch_add(&r->over_period, 1, __ATOMIC_RELAXED);
}

static inline void switch_gpu_comp_round_import(struct switch_gpu_comp_round *r, uint64_t ns)
{
	__atomic_fetch_add(&r->imports, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&r->import_ns, ns, __ATOMIC_RELAXED);
}

static inline void switch_gpu_comp_round_alloc(struct switch_gpu_comp_round *r, uint64_t ns,
					       int cold)
{
	__atomic_fetch_add(&r->allocs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&r->alloc_ns, ns, __ATOMIC_RELAXED);
	if (cold)
		__atomic_fetch_add(&r->cold_allocs, 1, __ATOMIC_RELAXED);
}

static inline void switch_gpu_comp_round_scanned(struct switch_gpu_comp_round *r, uint64_t passes,
						 uint64_t draws, uint64_t interlocks,
						 uint64_t twins)
{
	__atomic_fetch_add(&r->scanned, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&r->passes, passes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&r->draws, draws, __ATOMIC_RELAXED);
	__atomic_fetch_add(&r->interlocks, interlocks, __ATOMIC_RELAXED);
	__atomic_fetch_add(&r->twins, twins, __ATOMIC_RELAXED);
}

static inline void switch_gpu_comp_round_take(struct switch_gpu_comp_round *r,
					      struct switch_gpu_comp_round *out)
{
	out->jobs = __atomic_exchange_n(&r->jobs, 0, __ATOMIC_RELAXED);
	out->gpu_ns = __atomic_exchange_n(&r->gpu_ns, 0, __ATOMIC_RELAXED);
	out->over_period = __atomic_exchange_n(&r->over_period, 0, __ATOMIC_RELAXED);
	out->imports = __atomic_exchange_n(&r->imports, 0, __ATOMIC_RELAXED);
	out->import_ns = __atomic_exchange_n(&r->import_ns, 0, __ATOMIC_RELAXED);
	out->allocs = __atomic_exchange_n(&r->allocs, 0, __ATOMIC_RELAXED);
	out->alloc_ns = __atomic_exchange_n(&r->alloc_ns, 0, __ATOMIC_RELAXED);
	out->cold_allocs = __atomic_exchange_n(&r->cold_allocs, 0, __ATOMIC_RELAXED);
	out->scanned = __atomic_exchange_n(&r->scanned, 0, __ATOMIC_RELAXED);
	out->passes = __atomic_exchange_n(&r->passes, 0, __ATOMIC_RELAXED);
	out->draws = __atomic_exchange_n(&r->draws, 0, __ATOMIC_RELAXED);
	out->interlocks = __atomic_exchange_n(&r->interlocks, 0, __ATOMIC_RELAXED);
	out->twins = __atomic_exchange_n(&r->twins, 0, __ATOMIC_RELAXED);
}

/* The windows opened and channels held since the round before: what the counts gained since `seen` */
static inline void switch_gpu_comp_round_deadline(struct switch_gpu_comp_round *out,
						  struct switch_gpu_comp_deadline *seen,
						  uint32_t windows, uint32_t holds)
{
	out->windows = (uint32_t)(windows - seen->windows);
	out->holds = (uint32_t)(holds - seen->holds);
	seen->windows = windows;
	seen->holds = holds;
}

/* "jobs=N gpu_us=T over_period=O imports=I import_us=U allocs=A alloc_us=V cold_allocs=C scanned=S passes=P draws=D
 * ilk=L twin=W windows=O holds=H " */
static inline int switch_gpu_comp_round_format(char *buf, unsigned long len,
					       const struct switch_gpu_comp_round *r)
{
	return snprintf(
		buf, len,
		"jobs=%llu gpu_us=%llu over_period=%llu imports=%llu import_us=%llu allocs=%llu "
		"alloc_us=%llu cold_allocs=%llu scanned=%llu passes=%llu draws=%llu ilk=%llu "
		"twin=%llu windows=%llu holds=%llu ",
		(unsigned long long)r->jobs, (unsigned long long)(r->gpu_ns / 1000),
		(unsigned long long)r->over_period, (unsigned long long)r->imports,
		(unsigned long long)(r->import_ns / 1000), (unsigned long long)r->allocs,
		(unsigned long long)(r->alloc_ns / 1000), (unsigned long long)r->cold_allocs,
		(unsigned long long)r->scanned, (unsigned long long)r->passes,
		(unsigned long long)r->draws, (unsigned long long)r->interlocks,
		(unsigned long long)r->twins, (unsigned long long)r->windows,
		(unsigned long long)r->holds);
}

#endif
