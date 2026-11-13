/* SPDX-License-Identifier: MIT */
/* Compositor priority on the host's runlists (sched.c), the hold of app channels off the GPU ahead of a vblank
 * (hold.c) and when it happens (deadline.c). The compositor's channels and the GPU present's are high: the runlist lists
 * them after every other entry, and their kicks preempt the app channel the GPU is on. */
#pragma once
#include <stdint.h>

struct nvkm_device;

/* Once, after the FIFO exists (M2): every runlist is built interleaved from then on. */
void tgm_sched_init(struct nvkm_device *device);
/* A channel's runlist level (TegraGM20BSchedPolicy.h enum tgm_level): the compositor's channels are high, a
 * runningboardd role sets an app's background or foreground. A change rebuilds the runlist, so the host sees it at once. */
void tgm_sched_level(struct nvkm_device *device, unsigned int chid, unsigned int to);
/* Before nvkm removes a channel: its level goes back to foreground (nvkm's own runlist update then drops it) and the hold
 * forgets it. */
void tgm_sched_forget(struct nvkm_device *device, unsigned int chid);
/* Whether the host holds `chid`: on the PBDMA, or GR's channel or the one GR switches to (its RAMFC is the host's). */
int tgm_sched_loaded(struct nvkm_device *device, unsigned int chid);
/* What the GPU runs: the app channel on GR, else on the PBDMA; TGM_VICTIM_SWITCHING or TGM_VICTIM_NONE. */
int tgm_sched_resident(struct nvkm_device *device);

enum tgm_sched_kick {
	TGM_KICK_CLEAR,     /* the GPU was idle or on a high channel */
	TGM_KICK_PREEMPTED, /* an app channel held GR or the PBDMA: preempted */
	TGM_KICK_SWITCHING, /* the host was already switching (a timeslice expired or a preempt is in flight) */
	TGM_KICK_DEFERRED, /* an app channel held it, and nvkm held the runlist (a teardown or a recovery) */
	TGM_KICK_FOLLOWED, /* an app channel high work waits for held it: left to finish */
};

/* The app channel a high kick was queued behind, and the GPU's time (PTIMER) it was asked to leave: the submission's
 * start stamp, less that, is the channel's preemption latency. behind -1: none. */
struct tgm_sched_queued {
	int behind;
	uint64_t asked_pt;
};

/* A high channel kicked work: preempt the app channel holding GR or the PBDMA, if one does. Never waits. */
enum tgm_sched_kick tgm_sched_kicked(struct nvkm_device *device, unsigned int chid,
				     struct tgm_sched_queued *queued);
/* Debug kernels: this window's kicks by what they found, for the stats line. */
void tgm_sched_stats(char *buf, unsigned int len);

/* The hold (hold.c). Only channels gpu_chan.c registered (an app's) are held, never one errored, isolated or in recovery. */
struct tgm_window {
	uint64_t now, vblank, interval, expected;
};

void tgm_hold_init(void);
/* gpu_chan.c: an app channel was made (on) or is being torn down (off: released first, so its drain is not held). */
void tgm_hold_track(struct nvkm_device *device, unsigned int chid, int on);
/* gpu_hang.c: the channel is being isolated (its kill is under way): forgotten without a release (nvkm's recovery owns it). */
void tgm_hold_drop(unsigned int chid);
/* High work (the present, or a compositor submission through an acquire) waits for this channel's work: released at once,
 * and left alone until a present queued after this completes (it follows that work). */
void tgm_hold_need(struct nvkm_device *device, unsigned int chid);
int tgm_hold_needed(unsigned int chid);
/* A present queued at `queued` completed: what high work waited for before then is done. */
void tgm_hold_unneed(uint64_t queued);
/* Holds every channel due by w->now (its preemption latency before the compositor's latest start); returns the next
 * channel's hold time, 0 if none is left. *preempt: the held channel the GPU is on (resident), else -1. */
uint64_t tgm_hold_due(struct nvkm_device *device, const struct tgm_window *w, int resident,
		      int *preempt);
/* Asks the GPU to leave `chid` (PFIFO_PREEMPT, if nvkm holds no preempt of its own in flight): 1 if asked. */
int tgm_hold_preempt(struct nvkm_device *device, unsigned int chid);
/* A high kick found `chid` on the GPU and (asked_pt, the GPU's time now) asked it to leave, or found it already asked:
 * the time it was first asked, for the preemption latency sample. */
uint64_t tgm_hold_asked(unsigned int chid, uint64_t asked_pt);
/* The compositor's submission queued behind `chid` started at start_pt (its start stamp): an exact sample. */
void tgm_hold_sampled(unsigned int chid, uint64_t asked_pt, uint64_t start_pt);
/* Debug kernels: how many preemption latencies were sampled for process `pid`'s channels since its last call, the
 * longest, and where they landed with their buckets (SwitchGpuPreemptLanding.h; hw_test_app gpu.sched.long_draw_bands,
 * through SwitchGpuPathStats' line) */
struct switch_gpu_preempt_counts;
void tgm_hold_samples_take(int pid, uint32_t *n, uint64_t *max_ns,
			   struct switch_gpu_preempt_counts *counts);
/* Debug kernels: an app channel's batch ending at end_pt, the job's last or not: where a preempt lands */
void tgm_hold_released(unsigned int chid, uint64_t end_pt, int job_last);
/* Half its preemption latency after a preempt: if `chid` has left the GPU, the latency comes down. */
void tgm_hold_check(struct nvkm_device *device, unsigned int chid);
uint64_t tgm_hold_latency(unsigned int chid);
/* Whether any tracked app channel is slow enough to be held (tgm_hold_worth); lockless, for opening a window. */
int tgm_hold_wanted(void);
/* The window ends (the present completed, or it expired): every held channel is released. */
void tgm_hold_release(struct nvkm_device *device, uint64_t now);
/* The channel's cumulative time held, for the stuck timers (tgm_unheld_ns). */
uint64_t tgm_hold_held_ns(unsigned int chid, uint64_t now);
/* Debug kernels: the channel's preemption latency and the one-period turns it got since the last call. */
void tgm_hold_chan_stats(unsigned int chid, uint64_t *latency, unsigned int *turns);
void tgm_hold_stats(char *buf, unsigned int len);

/* The deadline (deadline.c). */
void tgm_deadline_init(struct nvkm_device *device);
/* gpu_exec.c: a high channel kicked (compositor: one of the compositor's, not the present's); app: it found an app on the GPU. */
void tgm_deadline_kicked(int compositor, int app);
/* gpu_fenced.h, interrupt pass: a high channel's fence moved past `ns` of GPU time (tgm_gpu_span); `biased` when that is
 * timed from a kick that found an app on the GPU (the frame's time is then not learned). */
void tgm_deadline_progress(uint64_t ns, int biased);
/* gpu_fenced.h, interrupt pass: both clocks now (PTIMER calibration), and a PTIMER span in nanoseconds (0 until calibrated). */
void tgm_deadline_clock(uint64_t ns, uint64_t pt);
uint64_t tgm_deadline_pt_ns(uint64_t pt_span);
/* gpu_fenced.h, interrupt pass: the present queued at `queued` completed. */
void tgm_deadline_presented(uint64_t queued);
/* gpu_power.c: system sleep and rail-down (every hold released, no callout touching the GPU until the wake or rail-up), and
 * the wake and rail-up. */
void tgm_deadline_suspend(void);
void tgm_deadline_resume(void);
void tgm_deadline_stats(char *buf, unsigned int len);
