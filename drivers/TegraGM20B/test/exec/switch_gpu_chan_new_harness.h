// SPDX-License-Identifier: MIT
// switch_gpu_chan_new_driver.c's model of the host's RAMFC for one channel. Plain C, for the C++ test.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_CHAN_NEW_NOUVEAU_ACQUIRE 0xfffff902u /* fifo/gk104.c gk104_chan_ramfc_write */
#define SWITCH_CHAN_NEW_ACQUIRE 0x7fff0041u         /* what tgm_acquire_arm writes, in the model */

struct switch_chan_new_ramfc {
	uint32_t timeslice, acquire;
};

struct switch_chan_new_host {
	uint32_t chid;
	struct switch_chan_new_ramfc memory; /* the instance block's */
	struct switch_chan_new_ramfc pbdma;  /* the loaded channel's copy */
	int loaded, enabled;
	uint32_t quiesced_waited; /* tgm_sched_ramfc calls that may wait for the runlist lock */
};

struct switch_chan_new_host *switch_chan_new_host(void);
/* tgm_chan_new on a client address space, the model's channel `chid`: 0 or its error */
int switch_chan_new_make(uint32_t chid, int critical);
int switch_chan_new_make_copy_kqueue(uint32_t chid, int high);
int switch_chan_new_app(void);
/* The timeslice the driver records as written (gpu_ramfc_probe.c's expected value), and the channel's teardown */
uint32_t switch_chan_new_recorded_timeslice(void);
void switch_chan_new_delete(void);
/* Work outstanding on the channel at its teardown, which kills it after the bounded wait (chan_abandon); whether it was
 * critical at that kill, -1 before one */
void switch_chan_new_leave_running(void);
int switch_chan_new_isolated_critical(void);
void switch_chan_new_set_visibility(uint32_t submitted, uint32_t completed, uint32_t visible);
uint32_t switch_chan_new_ltc_flushes(void);
/* tgm_chan_set_level at the given enum tgm_level (or high/foreground via switch_chan_new_set_level), and what the channel then is */
void switch_chan_new_set_level(int high, int critical);
void switch_chan_new_set_raw_level(uint32_t level, int critical);
int switch_chan_new_high(void);
int switch_chan_new_critical(void);
int switch_chan_new_busy_low(void);
void switch_chan_new_ramfc_due(void);
/* A job on the channel: the host saves what it holds, loads RAMFC from memory, runs it and saves it back. */
void switch_chan_new_job(void);

void *switch_exec_host_alloc(uint64_t bytes);
void switch_exec_host_free(void *block);

#ifdef __cplusplus
}
#endif
