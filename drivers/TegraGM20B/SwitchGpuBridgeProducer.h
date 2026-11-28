// SPDX-License-Identifier: MIT
// A bridge with no GPU job of its own (host1x.md A2): every pending token it is handed is a GM20B one on one channel,
// each inside a job of that channel that drained and incremented the channel's own syncpoint
// (SwitchGpuTimelineSyncpt.cpp), so the consumer waits on that. The bridge's state of such a slot comes from that
// channel at the last of those jobs (`covering`), never from the tokens alone.
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"

struct SwitchGpuProducerWait {
	uint32_t syncpoint = 0;
	uint32_t threshold = 0;
	switch_gpu_backing_token covering =
		{};               // a token of the producer's channel at its last covering job
	bool unassigned = false;  // `ask`'s channel has no syncpoint yet
	switch_gpu_backing_token ask = {};
};

// True when `wait` covers every token, so the bridge needs no GPU job. Takes the timelines' lock, not the bridge's.
bool switch_gpu_producer_wait(const switch_gpu_backing_token *tokens, uint32_t count,
			      SwitchGpuProducerWait &wait);
// Under the bridge's lock: gives the channel `wait` found without a syncpoint one, if one is free. One a gone timeline
// handed back first gets the increments its channel never ran, so the new owner's base is final.
void switch_gpu_producer_assign(const SwitchGpuProducerWait &wait);
// The covering job by its channel's fence: 1 drained, 0 not yet, -5 the channel ended short of it; *stopped once the
// channel ended, when no increment of its can land any more.
int switch_gpu_producer_covered(const switch_gpu_backing_token &covering, int *stopped);
// The hang rule's check of the covering job's channel (tgm_writer_check_progress); when the next check is due, 0 for
// none.
uint64_t switch_gpu_producer_check(const switch_gpu_backing_token &covering);
// The producer's channel stopped: the CPU makes the increments it owed, in thread context, after the jobs waiting on them
// were cancelled (tegra_host1x_syncpt_release_failed).
void switch_gpu_producer_release(uint32_t syncpoint, uint32_t threshold);
