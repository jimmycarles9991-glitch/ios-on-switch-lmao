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
	uint32_t push_bgn, push_cur, push_end, push_seg;
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
uint32_t switch_exec_chan_word_acquires(struct switch_exec_chan *waiter, uint32_t pool,
					uint32_t value);
uint32_t switch_exec_chan_syncpt_waits(struct switch_exec_chan *waiter, uint32_t id,
				       uint32_t value);
uint32_t switch_exec_parked_promises(void);
uint64_t switch_exec_mutex_takes(void);
int switch_exec_mutex_held(void);
void switch_exec_live_lock(void);
void switch_exec_live_unlock(void);
void switch_exec_chan_high(struct switch_exec_chan *chan, int high);
void switch_exec_chan_critical(struct switch_exec_chan *chan, int critical);
void switch_exec_chan_share_vm(struct switch_exec_chan *chan, struct switch_exec_chan *with);
void switch_exec_chan_kernel(struct switch_exec_chan *chan, int kernel);
void switch_exec_chan_dirty_tlb(struct switch_exec_chan *chan);
void switch_exec_chan_enable_visible(struct switch_exec_chan *chan);
void switch_exec_set_low_busy(uint32_t chid, int busy);
int switch_exec_chan_job_waits(struct switch_exec_chan *chan, uint32_t seqno);

struct switch_exec_wfi_counts {
	uint32_t method_wfis;
	uint32_t fb_flushes;
	uint32_t l2_flushes;
	uint32_t l2_invalidates; /* L2_SYSMEM_INVALIDATE: the L2's clean lines dropped */
	uint32_t release_wfi_en;
	uint32_t release_wfi_dis;
	uint32_t start_stamps_en;
	uint32_t start_stamps_dis;
	uint32_t end_stamps_en;
	uint32_t end_stamps_dis;
	uint32_t visible_wfi_en;
	uint32_t visible_wfi_dis;
	uint32_t fence_wfi_en;
	uint32_t fence_wfi_dis;
	uint32_t extra_wfi_en;
	uint32_t extra_wfi_dis;
	uint32_t syncpt_incrs;             /* SYNCPOINTB increments */
	uint32_t syncpt_incrs_after_fence; /* of those, pushed after the fence word's release */
	uint32_t
		syncpt_incrs_after_drain; /* of those, pushed after an L2 flush and the FB flush behind it */
	uint32_t syncpt_id;               /* the last increment's syncpoint */
};

void switch_exec_chan_count_wfis(struct switch_exec_chan *chan, uint32_t from_dword,
				 uint32_t to_dword, struct switch_exec_wfi_counts *out);

enum switch_exec_kick_kind {
	SWITCH_EXEC_KICK_CLEAR,
	SWITCH_EXEC_KICK_PREEMPTED,
	SWITCH_EXEC_KICK_SWITCHING,
	SWITCH_EXEC_KICK_DEFERRED,
};

struct switch_exec_later_stats {
	uint32_t kicks, frames, frame_max;
	uint64_t queue_max;
};

struct switch_exec_foreign_stats {
	uint32_t jobs, fences, promises, events, host1x, timed;
	uint64_t queue_sum, queue_max;
};

struct switch_exec_l2_inval_stats {
	uint32_t total, host_written, compositor, tlb, kernel_push;
};

void switch_exec_reset_driver(void);
/* The syncpoint switch_gpu_timeline_syncpt_next hands every channel (0: none), and the seqno it was last asked for */
void switch_exec_set_syncpt(uint32_t id);
uint32_t switch_exec_syncpt_marked(void);
void switch_exec_set_kick(enum switch_exec_kick_kind sched_kick, int deadline_later);
void switch_exec_set_ptimer(uint64_t pt);
void switch_exec_chan_complete_stamp(struct switch_exec_chan *chan, uint32_t seqno,
				     uint64_t start_pt, uint64_t end_pt);
void switch_exec_take_later_stats(struct switch_exec_later_stats *out);
void switch_exec_take_foreign_stats(struct switch_exec_foreign_stats *out);
void switch_exec_take_l2_inval_stats(struct switch_exec_l2_inval_stats *out);
/* The GPU clock on its idle divider until the next tgm_idle_raise, which calls `raised` first; NULL: the idle clock off */
void switch_exec_idle_low(void (*raised)(void *context), void *context);

void *switch_exec_host_alloc(uint64_t bytes);
void switch_exec_host_free(void *block);

#ifdef __cplusplus
}
#endif
