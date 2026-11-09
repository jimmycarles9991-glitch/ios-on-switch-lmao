/* SPDX-License-Identifier: MIT */
/* What the GPU is stuck on (recover.c), for gpu.c's client fault isolation. */
#pragma once
#include <stdint.h>
#include "TegraGM20BHost.h"

struct nvkm_device;
struct nvkm_fifo;
struct nvkm_fifo_func;

/* Once, after the FIFO exists (M2): recover.c takes PFIFO's scheduler errors and PBDMA ENG_RESET from nvkm's interrupt
 * handlers, and a context-switch timeout recovers the context only once its channel is stuck. */
void tgm_recover_init(struct nvkm_device *device);
/* PFIFO's engine context-switch timeout, after every FIFO init (bring-up and each wake): each period an engine's switch
 * stays pending raises SCHED_ERROR CTXSW_TIMEOUT. */
void tgm_recover_arm(struct nvkm_device *device);
/* Power-down, once the GPU's interrupts are off: no tick's work runs until the next one. */
void tgm_recover_suspend(void);

/* What an engine's switch that timed out (or that a scheduler error found pending) on hardware channel chid gets. */
enum tgm_ctxsw {
	TGM_CTXSW_WAIT, /* a live channel's work still drains: the next tick looks again */
	TGM_CTXSW_KILL, /* a live client's channel is stuck: it is killed, and nvkm's recovery faults the engine off it */
	TGM_CTXSW_RESET, /* the context is a killed channel's or no channel's: nvkm finds no channel group left to recover
			  * (nvkm_runl_rc_engn), so recover.c resets the engine */
	TGM_CTXSW_FATAL, /* a kernel channel is stuck */
};

/* known: one of gpu.c's channels; dead: killed already; stuck: tgm_stuck. */
static inline enum tgm_ctxsw tgm_ctxsw_verdict(int known, int dead, int kernel, int stuck)
{
	if (!known || dead)
		return TGM_CTXSW_RESET;
	if (!stuck)
		return TGM_CTXSW_WAIT;
	return kernel ? TGM_CTXSW_FATAL : TGM_CTXSW_KILL;
}

/* gpu.c, from recover.c's work (thread context): tgm_ctxsw_verdict for hardware channel chid. A stuck client channel is
 * being killed on return; a stuck kernel channel panics. */
enum tgm_ctxsw tgm_ctxsw_decide(int chid);

/* NV_PFIFO_INTR_SCHED_ERROR_CODE as NVIDIA's open gv100 dev_fifo.ref names it. gm20b's nvgpu header names only
 * CTXSW_TIMEOUT, 0x0a there as well, so the rest of Maxwell's encoding is taken to be the same. */
#define TGM_SCHED_CTXSW_TIMEOUT 0x0au

static inline const char *tgm_sched_error_name(uint32_t code)
{
	switch (code) {
	case 0x00:
		return "NO_ERROR";
	case 0x05:
		return "ENGINE_RESET";
	case 0x06:
		return "RL_ACK_TIMEOUT";
	case 0x07:
		return "RL_ACK_EXTRA";
	case 0x08:
		return "RL_RDAT_TIMEOUT";
	case 0x09:
		return "RL_RDAT_EXTRA";
	case TGM_SCHED_CTXSW_TIMEOUT:
		return "CTXSW_TIMEOUT";
	case 0x0c:
		return "RL_REQ_TIMEOUT";
	case 0x0d:
		return "NEW_RUNLIST";
	case 0x0e:
		return "CONFIG_WHILE_BUSY";
	case 0x20:
		return "BAD_TSG";
	default:
		return "unknown";
	}
}

/* NV_PFIFO_INTR_BIND_ERROR_CODE as nouveau's gk104_fifo_intr_bind_reason names it */
static inline const char *tgm_bind_error_name(uint32_t code)
{
	switch (code) {
	case 0x01:
		return "BIND_NOT_UNBOUND";
	case 0x02:
		return "SNOOP_WITHOUT_BAR1";
	case 0x03:
		return "UNBIND_WHILE_RUNNING";
	case 0x05:
		return "INVALID_RUNLIST";
	case 0x06:
		return "INVALID_CTX_TGT";
	case 0x0b:
		return "UNBIND_WHILE_PARKED";
	default:
		return "unknown";
	}
}

/* 1 if a busy engine (its PFIFO_ENG_STATUS) holds hardware channel chid's context: resident, saving it out (ID), or
 * loading it (NEXT_ID); CTX_STATUS 1 valid, 5 load, 6 save, 7 switch (nvgpu hw_fifo_gm20b.h, gk20a_fifo_engines_on_id). */
static inline int tgm_engine_holds(uint32_t status, int chid)
{
	uint32_t ctx = PFIFO_ENG_CTX_STATUS(status);
	int id = (ctx == 1 || ctx == 6 || ctx == 7) && !(status & PFIFO_ENG_ID_TSG) &&
		 (int)PFIFO_ENG_ID(status) == chid;
	int next = (ctx == 5 || ctx == 7) && !(status & PFIFO_ENG_NEXT_ID_TSG) &&
		   (int)PFIFO_ENG_NEXT_ID(status) == chid;

	return (status & PFIFO_ENG_BUSY) && (id || next);
}

/* PBDMA_INTR_0 ENG_RESET (gv100 dev_pbdma.ref): an engine of the PBDMA's runlist was reset while the PBDMA served a
 * channel. Only a recovery resets an engine, and only when the context on it, or the one it is switching out, is the
 * channel recovered (nvkm_runl_rc: the engine's channel group under recovery; recover.c: a killed channel or none). The
 * PBDMA's channel is then that one, or one whose context had not reached the engine, so none of its methods were
 * lost: the method it holds goes on to the reset engine, which loads its context first. nvgpu's gm20b PBDMA handling
 * counts no error for the bit; nvkm's would report it as the channel's error and replace that method with a NOP. */
#define TGM_PBDMA_ENG_RESET 0x01000000u

/* The PBDMA_INTR_0 bits nvkm's handler still gets. */
static inline uint32_t tgm_pbdma_for_nvkm(uint32_t intr0)
{
	return intr0 & ~TGM_PBDMA_ENG_RESET;
}

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
	int i = 0;

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

/* The faulting VA of nvkm's MMU fault report ("fault %02x [%s] at %016llx engine ... on channel %d [...]",
 * gf100_fifo_mmu_fault_recover) among a pass's reports (" | " apart): the one naming hardware channel chid, else one naming
 * no channel nvkm knew (-1). 0: neither. */
static inline uint64_t tgm_fault_va(const char *reports, int chid)
{
	uint64_t unnamed = 0;
	const char *p = reports;

	for (;;) {
		const char *end = __builtin_strstr(p, " | ");
		const char *fault = __builtin_strstr(p, "fault ");
		const char *at = __builtin_strstr(p, "] at ");
		const char *on = __builtin_strstr(p, "on channel ");
		uint64_t va = 0;
		int chan = 0;

		if (fault && at && on && fault < at && at < on && (!end || on < end)) {
			for (at += 5; (*at >= '0' && *at <= '9') || (*at >= 'a' && *at <= 'f');
			     at++)
				va = va << 4 | (uint64_t)(*at <= '9' ? *at - '0' : *at - 'a' + 10);
			on += 11;
			if (*on == '-') {
				unnamed = unnamed ? unnamed : va;
			} else {
				for (; *on >= '0' && *on <= '9'; on++)
					chan = chan * 10 + (*on - '0');
				if (chan == chid)
					return va;
			}
		}
		if (!end)
			return unnamed;
		p = end + 3;
	}
}
