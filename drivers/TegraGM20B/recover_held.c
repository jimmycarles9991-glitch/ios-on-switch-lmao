// SPDX-License-Identifier: MIT
/* What the engines and PBDMAs hold (TegraGM20BRecover.h), and the resets for it: a dead channel's residue, a stuck
 * kernel channel's engines (TegraGM20BKernelHang.h). */
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

static int add_once(int *chids, int n, int max, int chid)
{
	int i = 0;

	for (i = 0; i < n; i++) {
		if (chids[i] == chid)
			return n;
	}
	if (n < max)
		chids[n++] = chid;
	return n;
}

/* The runlist nvkm keeps hardware channel chid on, or NULL */
static struct nvkm_runl *chid_runl(struct nvkm_device *device, int chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *chan = NULL;
	unsigned long flags = 0;

	nvkm_runl_foreach(runl, device->fifo)
	{
		chan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (chan) {
			nvkm_chan_put(&chan, flags);
			return runl;
		}
	}
	return NULL;
}

/* gpu_reload.c, tgm_live_lock held */
int tgm_chid_live_client(int chid);
void tgm_chid_cut(int chid);

/* runl->mutex and tgm_live_lock held: resets the engines of runl that tgm_kernel_hang_resets names for hardware channel
 * chid (all: every one), each live kernel context they hold marked to be redone first; how many */
static int reset_for(struct nvkm_device *device, struct nvkm_runl *runl, int chid, int all,
		     const char *why)
{
	struct nvkm_engn *engn = NULL;
	int held[2];
	int holds_chid = 0;
	int live_client = 0;
	int reset = 0;
	int k = 0;
	int i = 0;
	u32 status = 0;

	/* nouveau's runlist walk takes each engine by container_of. */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
	{
		status = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
		k = tgm_engine_contexts_held(status, held);
		holds_chid = 0;
		live_client = tgm_engine_holds_tsg(status);
		for (i = 0; i < k; i++) {
			holds_chid |= held[i] == chid;
			live_client |= tgm_chid_live_client(held[i]);
		}
		if (!all && !tgm_kernel_hang_resets(status, holds_chid, live_client,
						    engn->engine->subdev.type == NVKM_ENGINE_CE))
			continue;
		for (i = 0; i < k; i++)
			tgm_chid_cut(held[i]);
		tgm_engine_reset_locked(runl, engn, chid, why);
		reset++;
	}
	return reset;
}

int tgm_recover_residue(struct nvkm_device *device, int chid)
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
			if (tgm_engine_holds(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)), chid)) {
				tgm_engine_reset_locked(runl, engn, chid, "still on dead");
				reset++;
			}
		}
		mutex_unlock(&runl->mutex);
	}
	return reset;
}

int tgm_recover_kernel(struct nvkm_device *device, int chid, int all)
{
	struct nvkm_runl *runl = chid_runl(device, chid);
	int reset = 0;

	if (!runl)
		return 0;
	mutex_lock(&runl->mutex);
	reset = reset_for(device, runl, chid, all, "reset under stuck kernel");
	mutex_unlock(&runl->mutex);
	return reset;
}

int tgm_recover_preempting(struct nvkm_device *device, int chid, int *its)
{
	const u32 preempt = nvkm_rd32(device, PFIFO_PREEMPT);
	const int pending = (preempt & PFIFO_PREEMPT_PENDING) != 0;
	const int it =
		pending && !(preempt & PFIFO_PREEMPT_TSG) && (int)PFIFO_PREEMPT_ID(preempt) == chid;

	*its = it;
	return pending && !it;
}

int tgm_recover_pbdma_chid(struct nvkm_device *device)
{
	struct nvkm_runq *runq = NULL;

	nvkm_runq_foreach(runq, device->fifo)
	{
		if (nvkm_rd32(device, PBDMA_STATUS(runq->id)) & PBDMA_BUSY)
			return (int)PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runq->id)));
	}
	return -1;
}

int tgm_recover_held(struct nvkm_device *device, int chid, int *chids, int max)
{
	struct nvkm_runl *runl = chid_runl(device, chid);
	struct nvkm_engn *engn = NULL;
	struct nvkm_runq *runq = NULL;
	int held[2];
	int n = 0;
	int k = 0;
	int i = 0;

	if (!runl)
		return 0;
	/* nouveau's runlist walk takes each engine by container_of. */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
	{
		k = tgm_engine_contexts_held(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)), held);
		for (i = 0; i < k; i++)
			n = add_once(chids, n, max, held[i]);
	}
	for (i = 0; i < runl->runq_nr; i++) {
		runq = runl->runq[i];
		if (runq && (nvkm_rd32(device, PBDMA_STATUS(runq->id)) & PBDMA_BUSY) &&
		    !tgm_pbdma_acquiring(nvkm_rd32(device, PBDMA_METHOD0(runq->id)),
					 nvkm_rd32(device, PBDMA_DATA0(runq->id))))
			n = add_once(chids, n, max,
				     PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runq->id))));
	}
	return n;
}

void tgm_recover_host_line(struct nvkm_device *device, char *buf, unsigned int len)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_runq *runq = NULL;
	unsigned int at = 0;

	at = scnprintf(buf, len, "host SCHED_DISABLE %08x ERROR_SCHED_DISABLE %08x PREEMPT %08x",
		       nvkm_rd32(device, PFIFO_SCHED_DISABLE),
		       nvkm_rd32(device, ERROR_SCHED_DISABLE), nvkm_rd32(device, PFIFO_PREEMPT));
	nvkm_runl_foreach(runl, device->fifo)
	{
		at += scnprintf(buf + at, len - at, ", runlist %d blocked %d rc %d", runl->id,
				runl->blocked, atomic_read(&runl->rc_pending));
	}
	nvkm_runq_foreach(runq, device->fifo)
	{
		at += scnprintf(buf + at, len - at, ", PBDMA%d INTR %08x EN %08x", runq->id,
				nvkm_rd32(device, PBDMA_INTR_0(runq->id)),
				nvkm_rd32(device, PBDMA_INTR_EN_0(runq->id)));
	}
}

int tgm_recover_resident(struct nvkm_device *device, int *chids, int max)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	struct nvkm_runq *runq = NULL;
	int held[2];
	int n = 0;
	int i = 0;
	int k = 0;

	nvkm_runl_foreach(runl, device->fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			k = tgm_engine_contexts(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)),
						held);
			for (i = 0; i < k; i++)
				n = add_once(chids, n, max, held[i]);
		}
		for (i = 0; i < runl->runq_nr; i++) {
			runq = runl->runq[i];
			if (runq && (nvkm_rd32(device, PBDMA_STATUS(runq->id)) & PBDMA_BUSY) &&
			    !tgm_pbdma_acquiring(nvkm_rd32(device, PBDMA_METHOD0(runq->id)),
						 nvkm_rd32(device, PBDMA_DATA0(runq->id))))
				n = add_once(
					chids, n, max,
					PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runq->id))));
		}
	}
	return n;
}
