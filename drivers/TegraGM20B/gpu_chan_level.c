// SPDX-License-Identifier: MIT
// A channel's level: its place on the runlist (sched.c) and its RAMFC words, the timeslice and the acquire timeout.
#include "gpu_internal.h"

/* Compositor priority: nvkm's runlist has no priorities ("TODO: prio, interleaving",
 * fifo/nv50.c nv50_runl_update) and gives every channel RAMFC's default timeslice, 128 << 3 (fifo/gk104.c
 * gk104_chan_ramfc_write, RAMFC 0xf8 = PFIFO 0x2310: enable 28, scale 15:12, timeout 7:0), as nvgpu's default
 * (gk20a/fifo_gk20a.c gk20a_fifo_default_timeslice_us). nvgpu gives high-priority work 4x its low timeslice
 * (os/linux/driver_common.c nvgpu_init_timeslice: 5200 vs 1300 us); critical channels get scale 5, 4x, from their
 * creation (tgm_chan_ramfc_open). RAMFC_TIMESLICE: gpu_internal.h. */
static int compositor_timeslice_on(void)
{
	return 1;
}

static void chan_timeslice(u32 chid, u32 value)
{
	tgm_ramfc_write(chid, RAMFC_TIMESLICE, value);
}

void tgm_chan_set_level(struct tgm_chan *chan, unsigned int level)
{
	int high = level == TGM_LEVEL_HIGH;
	u64 now = nvxnu_nanotime();
	u32 i = 0;

	if (chan->kernel)
		return;
	if (high == chan->high) {
		tgm_sched_level(tgm_gpu.device, chan->chid, level);
		return;
	}
	if (high) {
		/* work kicked before the change found no preempt: none of it is timed as the compositor's */
		for (i = 0; i < SWITCH_GPU_JOBS; i++) {
			WRITE_ONCE(chan->jobs.job[i].kick_ns, now);
			WRITE_ONCE(chan->jobs.job[i].behind, -1);
			WRITE_ONCE(chan->jobs.job[i].app, 1);
			WRITE_ONCE(chan->jobs.job[i].waits, 1);
		}
		WRITE_ONCE(chan->tracked, 0);
		chan->critical = 1;
		/* chan_completion reads the job records once it sees high */
		__atomic_store_n(&chan->high, 1, __ATOMIC_RELEASE);
		switch_gpu_busy_level(&tgm_busy, chan->chid, 0);
		tgm_sched_level(tgm_gpu.device, chan->chid, TGM_LEVEL_HIGH);
	} else {
		WRITE_ONCE(chan->high, 0);
		chan->critical = 0;
		switch_gpu_busy_level(&tgm_busy, chan->chid, 1);
		tgm_sched_level(tgm_gpu.device, chan->chid, level);
	}
	if (compositor_timeslice_on())
		chan->timeslice_due = high ? RAMFC_TIMESLICE_HIGH : RAMFC_TIMESLICE_DEFAULT;
}

struct ramfc_due {
	u32 chid, timeslice;
	int acquire;
};

static void ramfc_due_write(void *arg)
{
	struct ramfc_due *due = arg;

	if (due->timeslice)
		chan_timeslice(due->chid, due->timeslice);
	if (due->acquire)
		tgm_acquire_arm(due->chid);
}

/* A new channel is on its runlist and enabled when nvif_object_ctor returns (nvkm_uchan_init), so the host may already
 * hold its RAMFC, and the preempt of its first object's context bind would save that copy over a plain write (boot 22:
 * the scrubber's and 4 client channels' high timeslices read back as the default). Its first words go in as a level
 * change's do; channel creation waits for runl->mutex. */
int tgm_chan_ramfc_open(struct tgm_chan *chan, int critical)
{
	struct ramfc_due due = {chan->chid, 0, 1};
	int ret = 0;

	if (critical && tgm_all_clients() && compositor_timeslice_on())
		due.timeslice = RAMFC_TIMESLICE_HIGH;
	ret = tgm_sched_ramfc(tgm_gpu.device, chan->chid, ramfc_due_write, &due, 1);
	if (ret) {
		nvxnu_log("tegra-gm20b: channel %u (%s): not made, its RAMFC timeslice/acquire not "
			  "written: the preempt before it did not complete (PFIFO_PREEMPT 0x2634: "
			  "%d)\n",
			  chan->chid, chan->owner, ret);
		return ret;
	}
#if TGM_STATS
	if (due.timeslice)
		chan->timeslice = due.timeslice;
#endif
	return 0;
}

/* chan_exec, before a submission is kicked, with the channel's own work done: a level change's timeslice and the
 * acquire timeout an ACQUIRE interrupt turned off go into RAMFC after a preempt of the channel has completed
 * (tgm_sched_ramfc). The level setter and the acquire work hand them over through the due words; one not written
 * stays due for the next submission. */
void tgm_chan_ramfc_due(struct tgm_chan *chan)
{
	struct ramfc_due due = {chan->chid, 0, 0};
	u32 none = 0;
	int ret = 0;

	if ((!READ_ONCE(chan->timeslice_due) && !READ_ONCE(chan->acquire_due)) ||
	    (s32)(tgm_chan_completed(chan) - chan->submitted) < 0)
		return;
	due.timeslice = __atomic_exchange_n(&chan->timeslice_due, 0, __ATOMIC_ACQ_REL);
	due.acquire = __atomic_exchange_n(&chan->acquire_due, 0, __ATOMIC_ACQ_REL);
	ret = tgm_sched_ramfc(tgm_gpu.device, chan->chid, ramfc_due_write, &due, 0);
	if (ret) {
		if (due.timeslice) {
			__atomic_compare_exchange_n(&chan->timeslice_due, &none, due.timeslice,
						    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		}
		if (due.acquire)
			WRITE_ONCE(chan->acquire_due, 1);
		if (ret != -EBUSY) {
			nvxnu_log("tegra-gm20b: channel %u (%s): RAMFC timeslice/acquire not "
				  "written, "
				  "the preempt before it did not complete (PFIFO_PREEMPT 0x2634: "
				  "%d)\n",
				  chan->chid, chan->owner, ret);
		}
		return;
	}
#if TGM_STATS
	if (due.timeslice) {
		chan->timeslice = due.timeslice;
		chan->timeslice_at = chan->submitted;
		chan->timeslice_created = 0;
	}
#endif
}
