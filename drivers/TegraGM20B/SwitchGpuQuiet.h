/* SPDX-License-Identifier: MIT */
/* Quiet job ends (TegraGM20BJobQuiet): a job whose end nobody waits for releases its seqno without the nonstall
 * interrupt. A waiter asks for one before it sleeps (switch_gpu_quiet_need): a job kicked after the ask raises it at its
 * own end, and one kicked before is followed by a wake entry (a NON_STALL_INTERRUPT of its own GPFIFO entry) that the
 * channel's owner pushes under its submission order, so the interrupt comes at or after the seqno asked for.
 *
 * Order: an ask publishes its seqno, then reads what is announced; a job's fence reads the asks, then publishes what it
 * announces. Each side's fence between the two makes one of them see the other, so no ask is left behind both. */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"

/* announced: the newest seqno a pushed nonstall interrupt follows (the owner writes it); wanted: the newest seqno a
 * waiter asked for an interrupt at */
struct switch_gpu_quiet {
	uint32_t announced;
	uint32_t wanted;
};

static inline void switch_gpu_quiet_init(struct switch_gpu_quiet *q, uint32_t submitted)
{
	q->announced = submitted;
	q->wanted = submitted;
}

/* At a job's fence, by the submission's owner: whether the fence raises the nonstall interrupt. */
static inline int switch_gpu_quiet_announces(struct switch_gpu_quiet *q, int quiet)
{
	if (!quiet)
		return 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return (int32_t)(__atomic_load_n(&q->wanted, __ATOMIC_RELAXED) -
			 __atomic_load_n(&q->announced, __ATOMIC_RELAXED)) > 0;
}

/* Before the kick of a fence or wake entry that raises the interrupt behind `seqno`. */
static inline void switch_gpu_quiet_announce(struct switch_gpu_quiet *q, uint32_t seqno)
{
	__atomic_store_n(&q->announced, seqno, __ATOMIC_RELEASE);
}

/* A waiter of `seqno`, before it tests the fence and sleeps: 1 when no pushed interrupt follows that seqno yet, so the
 * owner has to be asked for a wake entry. */
static inline int switch_gpu_quiet_need(struct switch_gpu_quiet *q, uint32_t seqno)
{
	uint32_t seen = __atomic_load_n(&q->wanted, __ATOMIC_RELAXED);

	while ((int32_t)(seqno - seen) > 0 &&
	       !__atomic_compare_exchange_n(&q->wanted, &seen, seqno, 0, __ATOMIC_SEQ_CST,
					    __ATOMIC_RELAXED))
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return (int32_t)(seqno - __atomic_load_n(&q->announced, __ATOMIC_ACQUIRE)) > 0;
}

/* Whether a backing token is a seqno of the GM20B channel `channel_id` (its uid, and its reset generation above it), and
 * so asked of that channel. Another engine's token holds a host1x syncpoint in its queue, never a channel's uid. */
static inline int switch_gpu_quiet_token_on(const struct switch_gpu_backing_token *token,
					    uint64_t channel_id)
{
	return token->engine == SWITCH_GPU_ENGINE_GM20B && token->value <= UINT32_MAX &&
	       token->queue == (uint32_t)channel_id &&
	       token->reset_generation == (channel_id >> 32) + 1;
}

/* Whether a job asked to end quiet does. Not with a semantic fence bound to it (its batch is read again only at a
 * published completion), nor when it writes a backing the display scans in place and closes its admission group (a
 * command buffer range's last job, or its only one): that job is the backing's last writer, a watched present's
 * producer, whose own interrupt completes the frame where a wake entry would come behind every job queued after it. The
 * range's earlier jobs list the same writes and stay quiet. */
static inline int switch_gpu_quiet_end(int quiet, int semantic_fence, int scanout_writes,
				       int closes_group)
{
	return quiet && !semantic_fence && !(scanout_writes && closes_group);
}

enum {
	SWITCH_GPU_QUIET_UNASKED =
		0, /* nothing at or after the seqno interrupts, and no waiter asked */
	SWITCH_GPU_QUIET_OWED = 1, /* asked: the owner has not pushed the wake entry yet */
	SWITCH_GPU_QUIET_QUEUED =
		2, /* pushed, behind work the GPU has not done: the fence is short of *end */
	SWITCH_GPU_QUIET_DUE =
		3, /* the GPU is past *end, the fence the interrupt follows: its pass is due */
};

/* Where the interrupt for `seqno` is, with `completed` the channel's fence. *end comes in as
 * switch_gpu_jobs_announced_by's answer (`recorded` its flag). When later jobs overwrote the interrupting job's record,
 * *end becomes the newest announced seqno, which that interrupt cannot come after. */
static inline int switch_gpu_quiet_interrupt(const struct switch_gpu_quiet *q, uint32_t seqno,
					     uint32_t completed, uint32_t *end, int recorded)
{
	const uint32_t announced = __atomic_load_n(&q->announced, __ATOMIC_ACQUIRE);

	if ((int32_t)(announced - seqno) < 0)
		return (int32_t)(__atomic_load_n(&q->wanted, __ATOMIC_ACQUIRE) - seqno) >= 0
			       ? SWITCH_GPU_QUIET_OWED
			       : SWITCH_GPU_QUIET_UNASKED;
	if (!recorded)
		*end = announced;
	return (int32_t)(completed - *end) >= 0 ? SWITCH_GPU_QUIET_DUE : SWITCH_GPU_QUIET_QUEUED;
}

/* The owner, under its submission order, with `submitted` the channel's newest seqno: whether a wake entry is owed.
 * An ask past `submitted` belongs to a job not kicked yet, whose own fence sees it; the newest kicked job still gets
 * the entry, as an ask the max replaced may be for it. */
static inline int switch_gpu_quiet_wake_due(const struct switch_gpu_quiet *q, uint32_t submitted)
{
	const uint32_t announced = __atomic_load_n(&q->announced, __ATOMIC_RELAXED);

	return (int32_t)(submitted - announced) > 0 &&
	       (int32_t)(__atomic_load_n(&q->wanted, __ATOMIC_ACQUIRE) - announced) > 0;
}
