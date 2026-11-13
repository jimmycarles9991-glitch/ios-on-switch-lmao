/* SPDX-License-Identifier: MIT */
/* Debug kernels: the compositor's sync pushes (TGM_PC_SYNC: waits, cache invalidates, host flushes) in its profiled
 * submissions. Its first push is the raw path's submission prologue (a start stamp and cache invalidates, no acquire), whose
 * first graphics method waits for GR: when an app held GR at the kick (tgm_sched_kicked), until the app's context leaves at
 * its next wait for idle. So the prologue's time splits into kicks that found GR free and kicks behind an app, which is the
 * app's preemption latency, not the compositor's work. Plain C for the host test (test/gm20b_compositor_sync_test.cpp). */
#ifndef SWITCH_GPU_COMPOSITOR_SYNC_H
#define SWITCH_GPU_COMPOSITOR_SYNC_H
#include <stdint.h>
#include <string.h>

struct switch_gpu_comp_sync {
	uint32_t subs, behind, syncs;
	uint64_t sync_ns, behind_ns, max_ns;
};

/* One profiled submission: its sync pushes' count and time, whether its kick found an app on GR */
static inline void switch_gpu_comp_sync_add(struct switch_gpu_comp_sync *s, uint32_t syncs,
					    uint64_t sync_ns, int behind)
{
	__atomic_fetch_add(&s->subs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s->syncs, syncs, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s->sync_ns, sync_ns, __ATOMIC_RELAXED);
	if (behind) {
		__atomic_fetch_add(&s->behind, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&s->behind_ns, sync_ns, __ATOMIC_RELAXED);
	}
	uint64_t max = __atomic_load_n(&s->max_ns, __ATOMIC_RELAXED);
	while (sync_ns > max && !__atomic_compare_exchange_n(&s->max_ns, &max, sync_ns, 0,
							     __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

static inline void switch_gpu_comp_sync_take(struct switch_gpu_comp_sync *s,
					     struct switch_gpu_comp_sync *out)
{
	out->subs = __atomic_exchange_n(&s->subs, 0, __ATOMIC_RELAXED);
	out->behind = __atomic_exchange_n(&s->behind, 0, __ATOMIC_RELAXED);
	out->syncs = __atomic_exchange_n(&s->syncs, 0, __ATOMIC_RELAXED);
	out->sync_ns = __atomic_exchange_n(&s->sync_ns, 0, __ATOMIC_RELAXED);
	out->behind_ns = __atomic_exchange_n(&s->behind_ns, 0, __ATOMIC_RELAXED);
	out->max_ns = __atomic_exchange_n(&s->max_ns, 0, __ATOMIC_RELAXED);
}

/* "sync_subs=N syncs=K sync_us=T behind=B behind_sync_us=U sync_max_us=M": totals over the profiled submissions */
static inline int switch_gpu_comp_sync_format(char *buf, unsigned long len,
					      const struct switch_gpu_comp_sync *s)
{
	return snprintf(
		buf, len,
		"sync_subs=%u syncs=%u sync_us=%llu behind=%u behind_sync_us=%llu sync_max_us=%llu",
		s->subs, s->syncs, (unsigned long long)(s->sync_ns / 1000), s->behind,
		(unsigned long long)(s->behind_ns / 1000), (unsigned long long)(s->max_ns / 1000));
}

#endif
