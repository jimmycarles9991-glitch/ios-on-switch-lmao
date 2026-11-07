/* SPDX-License-Identifier: MIT */
/* What the GPU is stuck on (recover.c), for gpu.c's client fault isolation. */
#pragma once
#include <stdint.h>

struct nvkm_device;
struct nvkm_fifo;
struct nvkm_fifo_func;

/* Once, after the FIFO exists (M2): a context-switch timeout recovers the context only once its channel is stuck. */
void tgm_recover_init(struct nvkm_device *device);
/* PFIFO's engine context-switch timeout, after every FIFO init (bring-up and each wake): each period an engine's switch
 * stays pending raises SCHED_ERROR CTXSW_TIMEOUT. */
void tgm_recover_arm(struct nvkm_device *device);
/* gpu.c, from a tick (thread context): 1 if hardware channel chid is stuck and now being killed (a kernel channel's
 * panics), or is not one of gpu.c's; 0 to wait for the next tick. */
int tgm_ctxsw_stuck(int chid);
/* Fault every busy engine off hardware channel chid (as a context-switch timeout's recovery does), so a recovery of a
 * channel an engine hangs on does not wait for that engine to drain. */
void tgm_recover_fault(struct nvkm_device *device, int chid);
/* nvkm's FIFO functions as a writable copy put in place of its const table, once: recover.c and sched.c replace entries
 * of it, and vendored nouveau stays as it is. */
struct nvkm_fifo_func *tgm_fifo_func(struct nvkm_fifo *fifo);
/* Debug kernels: this window's context-switch timeout ticks waited out and recovered. */
void tgm_recover_stats(char *buf, unsigned int len);
/* The channels the GPU is on, most likely culprit first: each busy engine's (mid-switch, the context it saves or loads,
 * as nvkm's own recovery picks it), then each busy PBDMA's. Returns how many, at most max. */
int tgm_recover_suspects(struct nvkm_device *device, int *chids, int max);

/* A channel with work pending that has completed nothing for this long is stuck (one rule for the context-switch timeout
 * and every late wait). Above any finite drain seen (backboardd's two 144 ms m2v passes a frame), below TGM_HANG_NS and the
 * present's 1 s, so the channels a late wait finds stuck at its deadline were stuck for most of it. */
#define TGM_STUCK_NS (500ull * 1000 * 1000)

static inline int tgm_stuck(int dead, uint32_t completed, uint32_t submitted, uint64_t now,
			    uint64_t progress_ns)
{
	return !dead && (int32_t)(completed - submitted) < 0 && now - progress_ns >= TGM_STUCK_NS;
}

/* What a wait that ran out with no GPU error reported found (gpu.c hang_recover). */
enum tgm_hang {
	TGM_HANG_STUCK, /* nothing to recover: the GPU is stuck on a kernel channel, or nothing progresses: panic */
	TGM_HANG_RECOVERING, /* a client's channel the GPU is stuck on is being killed (or the late one died already) */
	TGM_HANG_PROGRESSING /* no channel is stuck: the late work is slow, not hung */
};

struct tgm_hang_candidate {
	uint8_t live; /* a channel of the kernel's list */
	uint8_t stuck; /* work pending and nothing completed for the stuck time (gpu.c TGM_STUCK_NS) */
	uint8_t kernel; /* the present's or the scrubber's */
};

/* Candidates in order: tgm_recover_suspects' channels, the channel of its task the late one waits on, the late one. The first
 * stuck one is to blame (*blame, its index; else -1): a client's is killed, a kernel channel's is fatal. With none stuck, a
 * dead late channel is recovered already, and late kernel work with nothing progressing anywhere is fatal. */
static inline enum tgm_hang tgm_hang_blame(const struct tgm_hang_candidate *c, int n, int late_dead,
					   int late_kernel, int any_progress, int *blame)
{
	int i;

	*blame = -1;
	for (i = 0; i < n; i++) {
		if (!c[i].live || !c[i].stuck)
			continue;
		if (c[i].kernel)
			return TGM_HANG_STUCK;
		*blame = i;
		return TGM_HANG_RECOVERING;
	}
	if (late_dead)
		return TGM_HANG_RECOVERING;
	if (late_kernel && !any_progress)
		return TGM_HANG_STUCK;
	return TGM_HANG_PROGRESSING;
}
