// SPDX-License-Identifier: MIT
#include "SwitchGpuJobStats.h"
#if SWITCH_GPU_JOB_STATS
#include "SwitchGpuResourceSet.h"

namespace
{
constexpr uint32_t kPhases = 4;  // TGM_SUBMIT_RESOLVE, _LOCK, _EXEC, _COMMIT

struct Window {
	uint64_t jobs, sets, stale, shared, sharedMax, siblings, total, totalMax;
	uint64_t ns[kPhases];
	uint64_t causes[SWITCH_GPU_SET_CAUSES];
} window;

void raise(uint64_t *max, uint64_t value)
{
	uint64_t seen = __atomic_load_n(max, __ATOMIC_RELAXED);
	while (value > seen && !__atomic_compare_exchange_n(max, &seen, value, false,
							    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

uint64_t take(uint64_t *counter)
{
	return __atomic_exchange_n(counter, 0, __ATOMIC_RELAXED);
}
}  // namespace

extern "C" void switch_gpu_job_submitted(const uint64_t *ns, int named_set, uint32_t shared_uses,
					 uint32_t siblings)
{
	uint64_t total = 0;
	for (uint32_t p = 0; p < kPhases; ++p) {
		__atomic_fetch_add(&window.ns[p], ns[p], __ATOMIC_RELAXED);
		total += ns[p];
	}
	__atomic_fetch_add(&window.jobs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&window.sets, named_set ? 1 : 0, __ATOMIC_RELAXED);
	__atomic_fetch_add(&window.shared, shared_uses, __ATOMIC_RELAXED);
	__atomic_fetch_add(&window.siblings, siblings, __ATOMIC_RELAXED);
	__atomic_fetch_add(&window.total, total, __ATOMIC_RELAXED);
	raise(&window.sharedMax, shared_uses);
	raise(&window.totalMax, total);
}

extern "C" void switch_gpu_job_stale(void)
{
	__atomic_fetch_add(&window.stale, 1, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_job_invalidated(uint32_t cause)
{
	if (cause < SWITCH_GPU_SET_CAUSES)
		__atomic_fetch_add(&window.causes[cause], 1, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_job_window(char *line, size_t size)
{
	uint64_t ns[kPhases], causes[SWITCH_GPU_SET_CAUSES];
	for (uint32_t p = 0; p < kPhases; ++p)
		ns[p] = take(&window.ns[p]);
	uint64_t changed = 0;
	for (uint32_t c = 0; c < SWITCH_GPU_SET_CAUSES; ++c)
		changed += causes[c] = take(&window.causes[c]);
	const uint64_t jobs = take(&window.jobs), sets = take(&window.sets),
		       stale = take(&window.stale);
	const uint64_t shared = take(&window.shared), sharedMax = take(&window.sharedMax);
	const uint64_t siblings = take(&window.siblings), total = take(&window.total);
	const uint64_t totalMax = take(&window.totalMax);
	if (!size)
		return;
	line[0] = 0;
	if (!jobs && !stale && !changed)
		return;
	const uint64_t n = jobs ? jobs : 1;
	// The kernel has no stdio.h: the builtin is the kernel's snprintf there and libc's on the host.
	__builtin_snprintf(line, size,
			   "SubmitJob %llu x mean %llu us (resolve %llu, lock %llu, exec %llu, "
			   "commit %llu) max %llu us, "
			   "set %llu hit %llu stale, shared %llu.%llu per job max %llu, siblings "
			   "%llu, generations ended: "
			   "free %llu layout %llu shared %llu kill %llu replace %llu cpu %llu",
			   jobs, total / n / 1000, ns[0] / n / 1000, ns[1] / n / 1000,
			   ns[2] / n / 1000, ns[3] / n / 1000, totalMax / 1000, sets, stale,
			   shared / n, shared * 10 / n % 10, sharedMax, siblings,
			   causes[SWITCH_GPU_SET_CAUSE_FREE], causes[SWITCH_GPU_SET_CAUSE_LAYOUT],
			   causes[SWITCH_GPU_SET_CAUSE_SHARED], causes[SWITCH_GPU_SET_CAUSE_KILL],
			   causes[SWITCH_GPU_SET_CAUSE_REPLACE], causes[SWITCH_GPU_SET_CAUSE_CPU]);
}
#endif
