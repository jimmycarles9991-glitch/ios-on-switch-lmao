/* SPDX-License-Identifier: MIT */
/* A kernel channel a hang check finds stuck (TegraGM20BRecover.h tgm_hang_blame's TGM_HANG_KERNEL): which engines are
 * reset for it, and when a stall is a hardware failure. gpu_reload.c tgm_kernel_hang_locked takes the steps. */
#pragma once
#include <stdint.h>
#include "TegraGM20BHost.h"

/* Steps for one stall, each at a hang check that still finds the channel stuck. A kernel channel can stop with no
 * engine switching (T-v5a: PBDMA0 on the scrubber's LAUNCH_DMA for seconds, GR idle on a client's context, no
 * context-switch timeout), so no step waits for an engine to look busy. */
enum tgm_kernel_hang {
	/* the engines of its runlist that hold its context or no live client's state (a copy engine's idle context is
	 * none), each under the hold that takes it off its PBDMA; nobody killed */
	TGM_KERNEL_HANG_OWN,
	/* every engine of its runlist, the live client channels whose contexts they hold or their PBDMA feeds killed first */
	TGM_KERNEL_HANG_ALL,
	/* an ALL reset with no client context resident, after which no channel completed anything and the channel never
	 * left its PBDMA: a hardware failure */
	TGM_KERNEL_HANG_FATAL,
	/* the last step's reset has had no time to show yet: every waiter that finds the channel stuck meanwhile (the
	 * scrubber's and the present's deadlines, CPU and IOSurface waits, kernel copy queues) waits on */
	TGM_KERNEL_HANG_WAIT,
};

/* What a stuck kernel channel's steps left (struct tgm_chan kernel_hang; tgm_live_lock) */
enum {
	TGM_KERNEL_HANG_NONE,
	TGM_KERNEL_HANG_DID_OWN,
	TGM_KERNEL_HANG_DID_ALL, /* with client contexts resident */
	TGM_KERNEL_HANG_DID_ALL_CLEAN,
};

struct tgm_kernel_hang_state {
	int did;
	uint32_t done; /* what the channel had completed at the last step */
	uint64_t ns;   /* that step's time, or a put-off one's */
	int put_off;   /* checks in a row that put the step off (tgm_kernel_hang_put_off) */
	int owns; /* OWN steps in a row that the channel completed nothing after, while other channels did */
	int pbdma;     /* the channel the PBDMA was on at the last step (-1: none) */
	int preempted; /* the last step's hold asked for the channel to leave its PBDMA */
};

/* A stall is TGM_STUCK_NS with nothing completed; a reset gets as long to show before the next step */
#define TGM_KERNEL_HANG_GAP_NS (500ull * 1000 * 1000)
/* The channel stuck again after this many OWN steps while other channels run (its own state is what does not come
 * back): the next step is ALL */
#define TGM_KERNEL_HANG_OWNS 3
/* Checks in a row a step waits for another channel's preempt to finish: the hold asks for none while one is in flight,
 * so a reset then would leave the stuck channel on its PBDMA. One still in flight after that is taken as never
 * finishing, and the step is taken anyway. */
#define TGM_KERNEL_HANG_WAITS 4

/* completed: what the channel has completed now; moved: the GPU went on after the last step (tgm_kernel_hang_moved).
 * The channel's own completion starts a new stall; the GPU going on means the reset freed it and the channel stalls on
 * its own state or anew, so not FATAL. */
static inline enum tgm_kernel_hang tgm_kernel_hang_next(const struct tgm_kernel_hang_state *s,
							uint32_t completed, int moved, uint64_t now)
{
	if (s->put_off && now - s->ns < TGM_KERNEL_HANG_GAP_NS)
		return TGM_KERNEL_HANG_WAIT;
	if (s->did == TGM_KERNEL_HANG_NONE || completed != s->done)
		return TGM_KERNEL_HANG_OWN;
	if (now - s->ns < TGM_KERNEL_HANG_GAP_NS)
		return TGM_KERNEL_HANG_WAIT;
	if (moved)
		return s->owns >= TGM_KERNEL_HANG_OWNS ? TGM_KERNEL_HANG_ALL : TGM_KERNEL_HANG_OWN;
	return s->did == TGM_KERNEL_HANG_DID_ALL_CLEAN ? TGM_KERNEL_HANG_FATAL
						       : TGM_KERNEL_HANG_ALL;
}

/* preempting: another channel's preempt is in flight. 1: the step is put off to a later check. */
static inline int tgm_kernel_hang_put_off(struct tgm_kernel_hang_state *s,
					  enum tgm_kernel_hang step, int preempting,
					  uint32_t completed, uint64_t now)
{
	if (completed != s->done)
		s->put_off = 0;
	if (!preempting || s->put_off >= TGM_KERNEL_HANG_WAITS)
		return 0;
	s->put_off++;
	s->ns = now;
	s->done = completed;
	if (step == TGM_KERNEL_HANG_OWN)
		s->did = TGM_KERNEL_HANG_NONE;
	return 1;
}

/* Whether the GPU went on after the last step: a live channel completed work (moved_ns, its last completion), the
 * channel left its PBDMA at that step's preempt (preempt_pending: one of the channel itself is still in flight), or the
 * PBDMA is on another channel now (pbdma) */
static inline int tgm_kernel_hang_moved(const struct tgm_kernel_hang_state *s, uint64_t moved_ns,
					int preempt_pending, int pbdma)
{
	return moved_ns > s->ns || (s->preempted && !preempt_pending) || pbdma != s->pbdma;
}

/* clients: the client contexts resident at an ALL step; pbdma: the channel the PBDMA was on; preempted: its hold asked
 * for the channel to leave its PBDMA */
static inline void tgm_kernel_hang_took(struct tgm_kernel_hang_state *s, enum tgm_kernel_hang step,
					int clients, int pbdma, int preempted, uint32_t completed,
					uint64_t now)
{
	if (step == TGM_KERNEL_HANG_OWN)
		s->owns = s->did != TGM_KERNEL_HANG_NONE && completed == s->done ? s->owns + 1 : 1;
	else
		s->owns = 0;
	if (step == TGM_KERNEL_HANG_OWN)
		s->did = TGM_KERNEL_HANG_DID_OWN;
	else
		s->did = clients ? TGM_KERNEL_HANG_DID_ALL : TGM_KERNEL_HANG_DID_ALL_CLEAN;
	s->done = completed;
	s->ns = now;
	s->put_off = 0;
	s->pbdma = pbdma;
	s->preempted = preempted;
}

/* The contexts an engine's status names whatever it is doing, BUSY or not: an idle engine's VALID context is still in
 * it, and a reset loses it. VALID, SAVE and SWITCH name ID; LOAD and SWITCH name NEXT_ID. out: up to two. */
static inline int tgm_engine_contexts_held(uint32_t status, int out[2])
{
	const uint32_t ctx = PFIFO_ENG_CTX_STATUS(status);
	int n = 0;

	if ((ctx == PFIFO_ENG_CTX_VALID || ctx == PFIFO_ENG_CTX_SAVE ||
	     ctx == PFIFO_ENG_CTX_SWITCH) &&
	    !(status & PFIFO_ENG_ID_TSG))
		out[n++] = (int)PFIFO_ENG_ID(status);
	if ((ctx == PFIFO_ENG_CTX_LOAD || ctx == PFIFO_ENG_CTX_SWITCH) &&
	    !(status & PFIFO_ENG_NEXT_ID_TSG))
		out[n++] = (int)PFIFO_ENG_NEXT_ID(status);
	return n;
}

/* A channel group's context on the engine: its channels are not named, so it counts as a live client's */
static inline int tgm_engine_holds_tsg(uint32_t status)
{
	const uint32_t ctx = PFIFO_ENG_CTX_STATUS(status);

	return ((ctx == PFIFO_ENG_CTX_VALID || ctx == PFIFO_ENG_CTX_SAVE ||
		 ctx == PFIFO_ENG_CTX_SWITCH) &&
		(status & PFIFO_ENG_ID_TSG)) ||
	       ((ctx == PFIFO_ENG_CTX_LOAD || ctx == PFIFO_ENG_CTX_SWITCH) &&
		(status & PFIFO_ENG_NEXT_ID_TSG));
}

/* Whether the OWN step resets an engine of the stuck kernel channel's runlist. holds_kernel: a context it holds is that
 * channel's; live_client: one is a live client's; copy: a copy engine, whose context keeps no state once it is idle. */
static inline int tgm_kernel_hang_resets(uint32_t status, int holds_kernel, int live_client,
					 int copy)
{
	if (holds_kernel || !live_client)
		return 1;
	return copy && !(status & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW));
}
