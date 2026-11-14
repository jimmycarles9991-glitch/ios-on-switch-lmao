/* SPDX-License-Identifier: MIT */
/* The live channels by hardware id, for the interrupt pass, which completes the channels tgm_busy shows with work in
 * flight (one nonstall interrupt for all; no per-channel event to arm or drop). A slot is written only at a channel's
 * creation, once the channel is whole, and at its teardown, which clears it and then hands off behind the pass
 * (nvxnu_pass_barrier: an empty action on the pass's work loop). A pass that read the slot before the clear has ended
 * before that action runs, so the channel it holds outlives its use; every pass after it reads the clear. */
#pragma once
#include <stdint.h>

#define SWITCH_GPU_CHAN_SLOTS 512u

struct switch_gpu_chan_table {
	void *chan[SWITCH_GPU_CHAN_SLOTS];
};

static inline void switch_gpu_chan_table_set(struct switch_gpu_chan_table *t, uint32_t chid,
					     void *chan)
{
	if (chid < SWITCH_GPU_CHAN_SLOTS) {
		__atomic_store_n(&t->chan[chid], chan, __ATOMIC_RELEASE);
	}
}

static inline void *switch_gpu_chan_table_get(const struct switch_gpu_chan_table *t, uint32_t chid)
{
	return chid < SWITCH_GPU_CHAN_SLOTS ? __atomic_load_n(&t->chan[chid], __ATOMIC_ACQUIRE) : 0;
}
