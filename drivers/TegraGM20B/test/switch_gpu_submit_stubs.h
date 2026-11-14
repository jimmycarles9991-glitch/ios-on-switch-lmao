// SPDX-License-Identifier: MIT
// The kernel the backing registry and the GM20B timeline engine call, for tests that run them together with no device:
// no surface, retirement on demand, event waits the test settles, and the fence words' holds counted.
#pragma once
#include <atomic>
#include <IOKit/IOUserClient.h>
#include "drivers/TegraGM20B/SwitchGpuBackingInternal.h"
#include "drivers/TegraGM20B/SwitchGpuTimelineInternal.h"

struct switch_gpu_event_waits {
	std::atomic<int> result{0};
};

// A CPU wait for a job (tgm_writer_wait) goes here when a test's GPU completes jobs; it returns at once otherwise.
inline void (*writerWait)(uint32_t queue, uint32_t value);

extern "C" {
struct tgm_coh;

tgm_coh *tgm_coh_find_pa(uint64_t)
{
	return nullptr;
}

void tgm_views_sweep(tgm_coh *)
{
}

void tgm_views_deferred_lock(uint32_t, uint32_t)
{
}

void tgm_coherency_put(tgm_coh *)
{
}

int tegra_gm20b_surface_tiled(uint64_t)
{
	return 0;
}

int switch_gpu_surface_prepare(uint32_t, void *, uint32_t, switch_gpu_backing **backing,
			       switch_gpu_cpu_lease **owned, void **descriptor)
{
	*backing = nullptr;
	*owned = nullptr;
	*descriptor = nullptr;
	return -19;
}

void switch_gpu_surface_finish(void *)
{
}

uint32_t switch_gpu_surface_users(void *)
{
	return 0;
}

void switch_gpu_surface_descriptor_hold(void *, int)
{
}

int switch_gpu_backing_prepare_retirement(void)
{
	return 0;
}

void switch_gpu_backing_schedule_retirement(void)
{
}

uint64_t switch_gpu_backing_time_ns(void)
{
	return 100;
}

void switch_gpu_backing_arm_expiry(uint64_t)
{
}

void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t, uint64_t, int)
{
}

void switch_gpu_event_waits_retain(switch_gpu_event_waits *)
{
}

void switch_gpu_event_waits_release(switch_gpu_event_waits **batch)
{
	*batch = nullptr;
}

int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *batch)
{
	return batch->result.load();
}

int switch_gpu_event_waits_wait(switch_gpu_event_waits *batch, uint64_t)
{
	return batch->result.load();
}

int switch_gpu_bridge_wait_external(const switch_gpu_backing_token *)
{
	return 1;
}

inline std::atomic<int> fenceHolds;

void switch_gpu_fence_hold(const volatile uint32_t *)
{
	++fenceHolds;
}

void switch_gpu_fence_put(const volatile uint32_t *)
{
	--fenceHolds;
}

void tgm_writer_wait(uint32_t queue, uint32_t value)
{
	if (writerWait)
		writerWait(queue, value);
}
}
