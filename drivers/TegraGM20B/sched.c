// SPDX-License-Identifier: MIT
/*
 * Compositor priority (TegraGM20BSched.h).
 * Maxwell preempts graphics only at a wait-for-idle, so a draw in flight always finishes; the host decides which channel
 * runs next and when it asks the running one to stop. nvkm builds a plain round robin (fifo/nv50.c nv50_runl_update:
 * "TODO: prio, interleaving"), so a compositor kick waited for every app with work to use up its timeslice. Here, as
 * nvgpu's interleave levels (gk20a_runlist_construct_locked: a higher level's entries repeat before each lower one), every
 * high channel follows each other entry of the runlist, so whichever app the host leaves, the compositor is next; and a
 * high channel's kick preempts the app channel GR or the PBDMA is on (PFIFO_PREEMPT, as gf100_chan_preempt), so the host
 * leaves it at its next wait-for-idle instead of at the end of its timeslice. Nothing waits for a preempt: a context that
 * never saves trips the engine's context-switch timeout, which recovers its channel (TegraGM20BRecover.h). Apps run at
 * runningboardd's background or foreground level below the compositor's, three levels interleaved the same way.
 */
#include <core/device.h>
#include <core/memory.h>
#include <engine/fifo.h>
#include <engine/fifo/runl.h>
#include <engine/fifo/runq.h>
#include <engine/fifo/cgrp.h>
#include <engine/fifo/chan.h>
#include <engine/fifo/chid.h>
#include <engine/fifo/priv.h> /* nv50_runl_update, nvkm_fifo_func */
#include <subdev/timer.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BHost.h"
#include "TegraGM20BRecover.h"
#include "TegraGM20BSched.h"
#include "TegraGM20BSchedPolicy.h"

#define SCHED_CHIDS 4096
#define SCHED_RUNLS 4

static u8 level[SCHED_CHIDS] = {[0 ... SCHED_CHIDS - 1] = TGM_LEVEL_FOREGROUND};

/* nvkm's channel functions, the preempt wrapped (sched_chan_preempt) */
static struct nvkm_chan_func chan_func;
static void (*orig_chan_preempt)(struct nvkm_chan *);

/* a runlist's units (a channel group: its header, if it has one, and its channels) and the order sched_update lists them in */
#define SCHED_UNITS 512
#define SCHED_ORDER 2048

static struct {
	struct nvkm_runl_func func; /* nvkm's, with sched_update */
	struct nvkm_memory *mem;
	u32 segment; /* bytes: two segments, one written while the host may still read the other */
	unsigned int next;
	struct nvkm_cgrp *unit[SCHED_UNITS];
	u8 level[SCHED_UNITS];
	u16 entries[SCHED_UNITS], order[SCHED_ORDER];
} rl[SCHED_RUNLS];

static struct {
	u32 kicks, preempted, switching, deferred, followed;
	u32 interleaved; /* runlists sched_update lists, for good */
} st;

static int is_high(int chid)
{
	return chid >= 0 && chid < SCHED_CHIDS && READ_ONCE(level[chid]) == TGM_LEVEL_HIGH;
}

static u8 cgrp_level(struct nvkm_cgrp *cgrp)
{
	struct nvkm_chan *chan = NULL;
	u8 top = TGM_LEVEL_BACKGROUND;

	nvkm_cgrp_foreach_chan(chan, cgrp)
	{
		if (chan->id >= 0 && chan->id < SCHED_CHIDS)
			top = max_t(u8, top, READ_ONCE(level[chan->id]));
	}
	return top;
}

/* runl->func->update, under runl->mutex as nvkm calls it. The wait for the host's last update comes first: it may run
 * nvkm's recovery (nvkm_runl_update_pending), which removes the channels it kills and updates the runlist itself, so the
 * groups listed after it are the ones left, and the segment written last time is free. */
static int sched_update(struct nvkm_runl *runl)
{
	const u32 size = runl->func->size;
	struct nvkm_memory *mem = rl[runl->id].mem;
	struct nvkm_cgrp **unit = rl[runl->id].unit;
	struct nvkm_cgrp *cgrp = NULL;
	struct nvkm_chan *chan = NULL;
	u32 units = 0;
	u32 n = 0;
	u32 i = 0;
	u32 start = 0;
	u32 offset = 0;

	if (runl->func->pending(runl))
		runl->func->wait(runl);
	if (runl->cgrp_nr > SCHED_UNITS)
		return nv50_runl_update(runl);
	nvkm_runl_foreach_cgrp(cgrp, runl)
	{
		if (units == SCHED_UNITS)
			break;
		unit[units] = cgrp;
		rl[runl->id].level[units] = cgrp_level(cgrp);
		rl[runl->id].entries[units++] = (int)cgrp->hw + cgrp->chan_nr;
	}
	n = tgm_interleave(rl[runl->id].level, rl[runl->id].entries, units,
			   rl[runl->id].segment / size, rl[runl->id].order, SCHED_ORDER);
	start = rl[runl->id].next * rl[runl->id].segment;
	rl[runl->id].next ^= 1;
	offset = start;
	nvkm_kmap(mem);
	for (i = 0; i < n; i++) {
		cgrp = unit[rl[runl->id].order[i]];
		if (cgrp->hw) {
			runl->func->insert_cgrp(cgrp, mem, offset);
			offset += size;
		}
		nvkm_cgrp_foreach_chan(chan, cgrp)
		{
			runl->func->insert_chan(chan, mem, offset);
			offset += size;
		}
	}
	nvkm_done(mem);
	list_rotate_left(&runl->cgrps);
	runl->func->commit(runl, mem, start, (offset - start) / size);
	return 0;
}

/* nvkm writes PFIFO_PREEMPT and waits for its own preempt to finish; one written over a high kick's preempt still in
 * flight (tgm_sched_kicked does not wait for its own) is lost, and nvkm would take the channel for preempted
 * (tgm_preempt_how). */
static void sched_chan_preempt(struct nvkm_chan *chan)
{
	struct nvkm_device *device = chan->cgrp->runl->fifo->engine.subdev.device;
	int pending = (nvkm_rd32(device, PFIFO_PREEMPT) & PFIFO_PREEMPT_PENDING) != 0;

	switch (tgm_preempt_how(irqs_disabled(), pending)) {
	case TGM_PREEMPT_RECOVER:
		nvkm_runl_rc_cgrp(chan->cgrp);
		return;
	case TGM_PREEMPT_AFTER_WAIT:
		/* nouveau's nvkm_wait_msec computes in int and widens the timeout. */
		/* NOLINTBEGIN(bugprone-signed-bitwise) */
		/* NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result) */
		nvkm_wait_msec(device, chan->cgrp->runl->fifo->timeout.chan_msec, PFIFO_PREEMPT,
			       PFIFO_PREEMPT_PENDING, 0);
		/* NOLINTEND(bugprone-implicit-widening-of-multiplication-result) */
		/* NOLINTEND(bugprone-signed-bitwise) */
		break;
	case TGM_PREEMPT_NOW:
		break;
	}
	orig_chan_preempt(chan);
}

/* 1 if `runl` now lists its channel groups with sched_update; its next update writes them so */
static int interleave(struct nvkm_device *device, struct nvkm_runl *runl)
{
	u32 entries = 0;

	if (runl->id < 0 || runl->id >= SCHED_RUNLS || rl[runl->id].mem ||
	    runl->func->update != nv50_runl_update || !runl->func->pending)
		return 0;
	/* nvkm's whole ring holds two of these: every entry once, and a high group after each low one */
	entries = 2 * ((runl->cgid ? runl->cgid->nr : 0) + runl->chid->nr);
	rl[runl->id].segment = ALIGN(entries * runl->func->size, 0x1000);
	if (nvkm_memory_new(device, NVKM_MEM_TARGET_INST, 2 * (u64)rl[runl->id].segment, 0x1000,
			    true, &rl[runl->id].mem)) {
		nvxnu_log("tegra-gm20b: runlist %d keeps nvkm's round robin: no memory\n",
			  runl->id);
		return 0;
	}
	mutex_lock(&runl->mutex);
	rl[runl->id].func = *runl->func;
	rl[runl->id].func.update = sched_update;
	runl->func = &rl[runl->id].func;
	atomic_set(&runl->changed, 1);
	mutex_unlock(&runl->mutex);
	if (TGM_STATS)
		__atomic_fetch_add(&st.interleaved, 1, __ATOMIC_RELAXED);
	return 1;
}

static int (*runl_ctor_nvkm)(struct nvkm_fifo *);

/* At M2 the FIFO has no user yet: nvkm makes its runlists at the first channel (nvkm_fifo_oneinit), before its first
 * init, so their first update waits for a channel to list. */
static int sched_runl_ctor(struct nvkm_fifo *fifo)
{
	struct nvkm_runl *runl = NULL;
	int ret = runl_ctor_nvkm(fifo);

	if (ret)
		return ret;
	nvkm_runl_foreach(runl, fifo)
	{
		interleave(fifo->engine.subdev.device, runl);
	}
	return 0;
}

void tgm_sched_init(struct nvkm_device *device)
{
	struct nvkm_fifo *fifo = device->fifo;
	struct nvkm_runl *runl = NULL;

	tgm_hold_init();
	tgm_deadline_init(device);
	if (fifo->func->chan.func->preempt && fifo->func->chan.func != &chan_func) {
		chan_func = *fifo->func->chan.func;
		orig_chan_preempt = chan_func.preempt;
		chan_func.preempt = sched_chan_preempt;
		tgm_fifo_func(fifo)->chan.func = &chan_func;
	}

	if (tgm_fifo_func(fifo)->runl_ctor != sched_runl_ctor) {
		runl_ctor_nvkm = fifo->func->runl_ctor;
		tgm_fifo_func(fifo)->runl_ctor = sched_runl_ctor;
	}
	nvkm_runl_foreach(runl, device->fifo)
	{
		if (interleave(device, runl)) {
			mutex_lock(&runl->mutex);
			nvkm_runl_update_locked(runl, true);
			mutex_unlock(&runl->mutex);
		}
	}
}

void tgm_sched_level(struct nvkm_device *device, unsigned int chid, unsigned int to)
{
	struct nvkm_runl *runl = NULL;

	if (chid >= SCHED_CHIDS || to >= TGM_LEVELS || READ_ONCE(level[chid]) == to)
		return;
	WRITE_ONCE(level[chid], to);
	if (to == TGM_LEVEL_HIGH)
		tgm_hold_track(device, chid, 0);
	nvkm_runl_foreach(runl, device->fifo)
	{
		if (runl->func->update != sched_update)
			continue;
		mutex_lock(&runl->mutex);
		atomic_set(&runl->changed, 1);
		nvkm_runl_update_locked(runl, true);
		mutex_unlock(&runl->mutex);
	}
}

void tgm_sched_forget(struct nvkm_device *device, unsigned int chid)
{
	if (chid >= SCHED_CHIDS)
		return;
	tgm_hold_track(device, chid, 0);
	WRITE_ONCE(level[chid], TGM_LEVEL_FOREGROUND);
}

static enum tgm_sched_kick counted(enum tgm_sched_kick kick)
{
	if (!TGM_STATS)
		return kick;
	__atomic_fetch_add(&st.kicks, 1, __ATOMIC_RELAXED);
	if (kick == TGM_KICK_PREEMPTED) {
		__atomic_fetch_add(&st.preempted, 1, __ATOMIC_RELAXED);
	} else if (kick == TGM_KICK_SWITCHING) {
		__atomic_fetch_add(&st.switching, 1, __ATOMIC_RELAXED);
	} else if (kick == TGM_KICK_DEFERRED) {
		__atomic_fetch_add(&st.deferred, 1, __ATOMIC_RELAXED);
	} else if (kick == TGM_KICK_FOLLOWED) {
		__atomic_fetch_add(&st.followed, 1, __ATOMIC_RELAXED);
	}
	return kick;
}

/* GR's engine status on the runlist (0: no GR on it) */
static u32 sched_gr_status(struct nvkm_device *device, struct nvkm_runl *runl)
{
	struct nvkm_engn *engn = NULL;

	nvkm_runl_foreach_engn(engn, runl)
	{
		if (engn->engine->subdev.type == NVKM_ENGINE_GR)
			return nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
	}
	return 0;
}

/* The app channel GR or the PBDMA serving the runlist is on (TegraGM20BSchedPolicy.h) */
static int sched_victim(struct nvkm_device *device, struct nvkm_runl *runl)
{
	u32 eng = sched_gr_status(device, runl);
	u32 pbdma = 0;
	u32 chid = 0;

	if (runl->runq_nr && runl->runq[0]) {
		pbdma = nvkm_rd32(device, PBDMA_STATUS(runl->runq[0]->id)) & PBDMA_BUSY;
		chid = PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runl->runq[0]->id)));
	}
	return tgm_kick_victim(eng, is_high(PFIFO_ENG_ID(eng)), pbdma != 0, chid, is_high(chid));
}

int tgm_sched_loaded(struct nvkm_device *device, unsigned int chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	u32 eng = 0;

	nvkm_runl_foreach(runl, device->fifo)
	{
		if (runl->runq_nr && runl->runq[0] &&
		    PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runl->runq[0]->id))) == chid)
			return 1;
		/* Only engines with a context have a status register: SW's id 15 reads 0x0026b8, which PRI-times out */
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			eng = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
			if (!(eng & PFIFO_ENG_ID_TSG) && PFIFO_ENG_ID(eng) == chid)
				return 1;
			if (!(eng & PFIFO_ENG_NEXT_ID_TSG) && PFIFO_ENG_NEXT_ID(eng) == chid)
				return 1;
		}
	}
	return 0;
}

int tgm_sched_resident(struct nvkm_device *device)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;

	nvkm_runl_foreach(runl, device->fifo)
	{
		if (runl->func->update != sched_update)
			continue;
		nvkm_runl_foreach_engn(engn, runl)
		{
			if (engn->engine->subdev.type == NVKM_ENGINE_GR)
				return sched_victim(device, runl);
		}
	}
	return TGM_VICTIM_NONE;
}

/* The kick's submission waits for `victim` to leave: the time it was first asked to, by this kick or a hold before it. */
static enum tgm_sched_kick queued_behind(struct nvkm_device *device, enum tgm_sched_kick kick,
					 int victim, struct tgm_sched_queued *queued)
{
	u64 asked = kick == TGM_KICK_PREEMPTED ? nvkm_timer_read(device->timer) : 0;

	queued->behind = -1;
	if (victim >= 0) {
		queued->asked_pt = tgm_hold_asked(victim, asked);
		if (queued->asked_pt)
			queued->behind = victim;
	}
	return counted(kick);
}

enum tgm_sched_kick tgm_sched_kicked(struct nvkm_device *device, unsigned int chid,
				     struct tgm_sched_queued *queued)
{
	struct nvkm_runl *runl = NULL;
	int victim = 0;

	queued->behind = -1;
	nvkm_runl_foreach(runl, device->fifo)
	{
		if (runl->func->update != sched_update || !is_high(chid) ||
		    (int)chid >= runl->chid->nr || !test_bit(chid, runl->chid->used))
			continue;
		victim = sched_victim(device, runl);
		if (victim == TGM_VICTIM_SWITCHING) {
			const u32 eng = sched_gr_status(device, runl);

			victim = tgm_kick_loading(eng, is_high(PFIFO_ENG_NEXT_ID(eng)));
		}
		if (victim == TGM_VICTIM_SWITCHING)
			return counted(TGM_KICK_SWITCHING);
		if (victim == TGM_VICTIM_NONE)
			return counted(TGM_KICK_CLEAR);
		if (!tgm_kick_may_preempt(victim, tgm_hold_needed(victim)))
			return counted(TGM_KICK_FOLLOWED);
		/* nvkm writes PFIFO_PREEMPT under runl->mutex and waits for it there; one of its own is in flight */
		if (!mutex_trylock(&runl->mutex))
			return queued_behind(device, TGM_KICK_DEFERRED, victim, queued);
		if (nvkm_rd32(device, PFIFO_PREEMPT) & PFIFO_PREEMPT_PENDING) {
			mutex_unlock(&runl->mutex);
			return queued_behind(device, TGM_KICK_SWITCHING, victim, queued);
		}
		nvkm_wr32(device, PFIFO_PREEMPT, victim);
		mutex_unlock(&runl->mutex);
		return queued_behind(device, TGM_KICK_PREEMPTED, victim, queued);
	}
	return TGM_KICK_CLEAR;
}

void tgm_sched_stats(char *buf, unsigned int len)
{
	u32 kicks = __atomic_exchange_n(&st.kicks, 0, __ATOMIC_RELAXED);
	u32 preempted = __atomic_exchange_n(&st.preempted, 0, __ATOMIC_RELAXED);
	u32 switching = __atomic_exchange_n(&st.switching, 0, __ATOMIC_RELAXED);
	u32 deferred = __atomic_exchange_n(&st.deferred, 0, __ATOMIC_RELAXED);
	u32 followed = __atomic_exchange_n(&st.followed, 0, __ATOMIC_RELAXED);

	scnprintf(buf, len,
		  "%u runlists interleaved, %u high kicks: %u preempted an app, %u found a switch "
		  "under "
		  "way, %u deferred, %u left the app they wait for running",
		  __atomic_load_n(&st.interleaved, __ATOMIC_RELAXED), kicks, preempted, switching,
		  deferred, followed);
}
