// SPDX-License-Identifier: MIT
// The channel epoch's count (SwitchGpuChannelEpoch.h) and its value in the fence pages.
#include "gpu_internal.h"
#include "SwitchGpuChannelEpoch.h"

/* A leaf spinlock: the pool's free path moves the epoch from any thread that gives pages back, holding locks of its own
 * (tgm_gpu.lock in a channel's teardown, the interrupt pass's scrubber), so nothing sleeps or is taken inside it. */
static DEFINE_SPINLOCK(epoch_lock);
static uint32_t epoch;
uint32_t switch_gpu_channel_memory_refused, switch_gpu_channel_creates;

void switch_gpu_channel_epoch_move(void)
{
	spin_lock(&epoch_lock);
	switch_gpu_channel_epoch_mirror(&epoch, tgm_fence_pages.va, TGM_FENCE_USER_PAGES,
					TGM_FENCE_SLOT);
	spin_unlock(&epoch_lock);
}

void switch_gpu_channel_epoch_fill(volatile uint32_t *page)
{
	spin_lock(&epoch_lock);
	switch_gpu_channel_epoch_store(page, epoch);
	spin_unlock(&epoch_lock);
}
