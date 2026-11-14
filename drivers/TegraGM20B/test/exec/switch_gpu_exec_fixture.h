// SPDX-License-Identifier: MIT
// gpu_exec.c's channels on the host: a real nvif 906f channel (push buffer, GPFIFO ring, USERD and GET semaphore in
// host memory) whose GPFIFO entry writes, doorbell and GET reads call the test first. Plain C, for the C++ test.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tgm_chan;
struct switch_exec_chan;

struct switch_exec_hooks {
	void *context;
	void (*entry)(void *context, struct switch_exec_chan *chan, int main, uint64_t addr,
		      uint32_t bytes);
	void (*doorbell)(void *context, struct switch_exec_chan *chan, uint32_t put);
	/* before the read, so the hook may move what it returns; calls counts the channel's reads of that GET */
	void (*read_get)(void *context, struct switch_exec_chan *chan, int gpfifo, uint32_t calls);
};

/* Push buffer positions are in dwords from its start. gp_put is what the last doorbell wrote to USERD. */
struct switch_exec_chan_pointers {
	uint32_t gpfifo_cur, gp_put, submitted, push_max;
	int32_t gpfifo_free;
	uint32_t push_bgn, push_cur, push_end;
	int defer_kick;
};

void switch_exec_reset(const struct switch_exec_hooks *hooks);
struct switch_exec_chan *switch_exec_chan_new(uint32_t uid, uint32_t fence_page);
struct tgm_chan *switch_exec_chan_tgm(struct switch_exec_chan *chan);
void switch_exec_chan_live(struct switch_exec_chan *chan, int live);
void switch_exec_chan_set_get(struct switch_exec_chan *chan, uint32_t push_get,
			      uint32_t gpfifo_get);
void switch_exec_chan_place(struct switch_exec_chan *chan, uint32_t push_dwords,
			    uint32_t gpfifo_cur, int32_t gpfifo_free);
void switch_exec_chan_read(struct switch_exec_chan *chan, struct switch_exec_chan_pointers *out);
/* The ACQ_GEQ acquires of producer's fence word reaching value in waiter's push buffer */
uint32_t switch_exec_chan_acquires(struct switch_exec_chan *waiter,
				   struct switch_exec_chan *producer, uint32_t value);
uint32_t switch_exec_parked_promises(void);

void *switch_exec_host_alloc(uint64_t bytes);
void switch_exec_host_free(void *block);

#ifdef __cplusplus
}
#endif
