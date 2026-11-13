// SPDX-License-Identifier: MIT
/* Engine resets (TegraGM20BRecover.h): the PBDMA's ENG_RESET held across each one, and the resets under a context an
 * engine cannot load or save. */
#include <core/device.h>
#include <core/engine.h>
#include <engine/fifo.h>
#include <engine/fifo/runl.h>
#include <engine/fifo/runq.h>
#include <engine/fifo/chan.h>
#include <engine/fifo/cgrp.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BHost.h"
#include "TegraGM20BRecover.h"

#define PGRAPH_STATUS 0x400700

struct hold_note {
	u32 eng, pbdma, method0, data0, chid;
	int engine;
	enum tgm_reset_pbdma pbdma_was;
	int preempted;
	u64 ns;
};

static struct {
	u32 holds; /* engine resets under way (device->intr.lock) */
	u32 resets;
	struct hold_note note; /* the outermost hold's PBDMA and engine (device->intr.lock) */
} rst;

/* A channel nvkm still has and is not recovering or has not errored. */
static bool chan_alive(struct nvkm_fifo *fifo, u32 chid)
{
	unsigned long flags = 0;
	struct nvkm_chan *chan = nvkm_chan_get_chid(&fifo->engine, (int)chid, &flags);
	bool alive = false;

	if (chan) {
		alive = !atomic_read(&chan->errored) &&
			atomic_read(&chan->cgrp->rc) == NVKM_CGRP_RC_NONE;
		nvkm_chan_put(&chan, flags);
	}
	return alive;
}

static int engine_id(struct nvkm_fifo *fifo, struct nvkm_engine *engine)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;

	nvkm_runl_foreach(runl, fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->engine == engine && engn->func->cxid)
		{
			return engn->id;
		}
	}
	return -1;
}

/* device->intr.lock held. A killed channel's method would make the reset engine load that channel's context, a load
 * that does not finish: replaced with a NOP, as nvkm's PBDMA handler replaces a faulting one. */
static void eng_reset_ack(struct nvkm_device *device, struct nvkm_runq *runq, const char *when)
{
	const u32 chid = PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runq->id)));
	const bool dead = !chan_alive(runq->fifo, chid);

	if (dead)
		nvkm_wr32(device, PBDMA_METHOD0(runq->id), PBDMA_METHOD_NOP);
	nvkm_wr32(device, PBDMA_INTR_0(runq->id), TGM_PBDMA_ENG_RESET);
	nvxnu_log("tegra-gm20b: PBDMA%d was on channel %u when %s (ENG_RESET): cleared%s\n",
		  runq->id, chid, when, dead ? ", the killed channel's method dropped" : "");
}

void tgm_eng_reset_intr(struct nvkm_device *device, struct nvkm_runq *runq)
{
	eng_reset_ack(device, runq, "an engine was reset with no recovery holding ENG_RESET");
}

/* device->intr.lock held: the outermost hold masks ENG_RESET on every PBDMA and asks the host to take a live channel off
 * it (PFIFO_PREEMPT, as gf100_chan_preempt writes it), unless a preempt is already pending. */
static void hold_pbdmas(struct nvkm_device *device, struct nvkm_engine *engine)
{
	struct nvkm_runq *runq = NULL;
	struct hold_note *note = &rst.note;

	note->engine = engine_id(device->fifo, engine);
	note->eng = note->engine >= 0 ? nvkm_rd32(device, PFIFO_ENG_STATUS(note->engine)) : 0;
	note->ns = nvxnu_nanotime();
	nvkm_runq_foreach(runq, device->fifo)
	{
		const u32 status = nvkm_rd32(device, PBDMA_STATUS(runq->id));
		const u32 chid = PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runq->id)));
		const enum tgm_reset_pbdma was =
			tgm_reset_pbdma(status, chan_alive(device->fifo, chid));

		nvkm_mask(device, PBDMA_INTR_EN_0(runq->id), TGM_PBDMA_ENG_RESET, 0);
		note->preempted = was == TGM_RESET_PBDMA_LIVE &&
				  !(nvkm_rd32(device, PFIFO_PREEMPT) & PFIFO_PREEMPT_PENDING);
		if (note->preempted)
			nvkm_wr32(device, PFIFO_PREEMPT, chid);
		note->pbdma = status;
		note->chid = chid;
		note->pbdma_was = was;
		note->method0 = nvkm_rd32(device, PBDMA_METHOD0(runq->id));
		note->data0 = nvkm_rd32(device, PBDMA_DATA0(runq->id));
	}
}

void tgm_recover_reset_hold(struct nvkm_engine *engine)
{
	struct nvkm_device *device = engine->subdev.device;
	unsigned long flags = 0;

	if (!device->fifo)
		return;
	spin_lock_irqsave(&device->intr.lock, flags);
	if (tgm_reset_hold_enter(&rst.holds))
		hold_pbdmas(device, engine);
	spin_unlock_irqrestore(&device->intr.lock, flags);
}

#if TGM_STATS
static const char *const pbdma_was_name[] = {
	[TGM_RESET_PBDMA_IDLE] = "no channel",
	[TGM_RESET_PBDMA_LIVE] = "live",
	[TGM_RESET_PBDMA_DEAD] = "killed",
};

static void release_line(const struct hold_note *note, u32 at_release, u32 after_ack, u32 pgraph,
			 int raised)
{
	nvxnu_log("tegra-gm20b: engine %d reset under PBDMA's channel %u (%s%s): ENG %08x at the "
		  "hold, %08x at the release, %08x after ENG_RESET's ack (%s); PGRAPH %08x; "
		  "PBDMA %08x mthd %08x data %08x at the hold; held %llu us\n",
		  note->engine, note->chid, pbdma_was_name[note->pbdma_was],
		  note->pbdma_was != TGM_RESET_PBDMA_LIVE ? ""
		  : note->preempted                       ? ", preempted"
							  : ", a preempt already pending",
		  note->eng, at_release, after_ack, raised ? "raised" : "not raised", pgraph,
		  note->pbdma, note->method0, note->data0, (nvxnu_nanotime() - note->ns) / 1000);
}
#endif

/* The outermost release, once the engine is back: ENG_RESET acknowledged and unmasked on every PBDMA. */
void tgm_recover_reset_release(struct nvkm_engine *engine)
{
	struct nvkm_device *device = engine->subdev.device;
	struct nvkm_runq *runq = NULL;
	struct hold_note note = {0};
	unsigned long flags = 0;
	u32 at_release = 0, after_ack = 0;
	int raised = 0;
	int last = 0;

	if (!device->fifo)
		return;
	spin_lock_irqsave(&device->intr.lock, flags);
	last = tgm_reset_hold_leave(&rst.holds);
	if (last) {
		note = rst.note;
		at_release =
			note.engine >= 0 ? nvkm_rd32(device, PFIFO_ENG_STATUS(note.engine)) : 0;
		nvkm_runq_foreach(runq, device->fifo)
		{
			if (nvkm_rd32(device, PBDMA_INTR_0(runq->id)) & TGM_PBDMA_ENG_RESET) {
				eng_reset_ack(device, runq, "a recovery reset its engine");
				raised = 1;
			}
			nvkm_mask(device, PBDMA_INTR_EN_0(runq->id), TGM_PBDMA_ENG_RESET,
				  TGM_PBDMA_ENG_RESET);
		}
		after_ack = note.engine >= 0 ? nvkm_rd32(device, PFIFO_ENG_STATUS(note.engine)) : 0;
	}
	spin_unlock_irqrestore(&device->intr.lock, flags);
#if TGM_STATS
	if (last)
		release_line(&note, at_release, after_ack, nvkm_rd32(device, PGRAPH_STATUS),
			     raised);
#else
	(void)at_release;
	(void)after_ack;
	(void)raised;
#endif
}

void tgm_engine_reset_locked(struct nvkm_runl *runl, struct nvkm_engn *engn, int id,
			     const char *why)
{
	nvxnu_log("tegra-gm20b: engine %d %s channel %d: reset\n", engn->id, why, id);
	nvkm_runl_block(runl);
	tgm_recover_reset_hold(engn->engine);
	WARN_ON(nvkm_engine_reset(engn->engine));
	tgm_recover_reset_release(engn->engine);
	if (runl->func->fault_clear)
		runl->func->fault_clear(runl);
	nvkm_runl_allow(runl);
	if (TGM_STATS)
		__atomic_fetch_add(&rst.resets, 1, __ATOMIC_RELAXED);
}

u32 tgm_engine_resets_take(void)
{
	return __atomic_exchange_n(&rst.resets, 0, __ATOMIC_RELAXED);
}

static int loads(u32 status, int chid)
{
	return tgm_engine_load_stuck(status) && !(status & PFIFO_ENG_NEXT_ID_TSG) &&
	       (int)PFIFO_ENG_NEXT_ID(status) == chid;
}

const char *tgm_recover_loading(struct nvkm_device *device, int chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;

	nvkm_runl_foreach(runl, device->fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			if (loads(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)), chid))
				return engn->engine->subdev.name;
		}
	}
	return NULL;
}

int tgm_recover_reload(struct nvkm_device *device, int chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	int reset = 0;

	nvkm_runl_foreach(runl, device->fifo)
	{
		mutex_lock(&runl->mutex);
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			if (loads(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)), chid)) {
				tgm_engine_reset_locked(runl, engn, chid,
							"stuck loading the context of live");
				reset++;
			}
		}
		mutex_unlock(&runl->mutex);
	}
	return reset;
}

/* gpu_fault.c, runl->mutex held: hardware channel chid is a live client's with nothing pending */
int tgm_chid_unowned(int chid);

void tgm_recover_preempt_unblock(struct nvkm_runl *runl)
{
	struct nvkm_device *device = runl->fifo->engine.subdev.device;
	struct nvkm_engn *engn = NULL;
	u32 status = 0;
	int id = 0;

	if (!(nvkm_rd32(device, PFIFO_PREEMPT) & PFIFO_PREEMPT_PENDING))
		return;
	/* nouveau's runlist walk takes each engine by container_of. */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
	{
		status = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
		id = tgm_engine_saving(status,
				       PFIFO_ENG_CTX_STATUS(status) == PFIFO_ENG_CTX_SWITCH &&
					       nvkm_engine_chsw_load(engn->engine));
		if (id >= 0 && (!chan_alive(runl->fifo, (u32)id) || tgm_chid_unowned(id)))
			tgm_engine_reset_locked(runl, engn, id,
						"saving, with nothing to drain, the context of");
	}
}

int tgm_recover_saving(struct nvkm_device *device, int chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	u32 status = 0;

	nvkm_runl_foreach(runl, device->fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			status = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
			if (tgm_engine_saving(
				    status, PFIFO_ENG_CTX_STATUS(status) == PFIFO_ENG_CTX_SWITCH &&
						    nvkm_engine_chsw_load(engn->engine)) == chid)
				return 1;
		}
	}
	return 0;
}
