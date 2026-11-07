// SPDX-License-Identifier: MIT
/* See TegraGM20BRecover.h. */
#include <core/device.h>
#include <engine/fifo.h>
#include <engine/fifo/runl.h>
#include <engine/fifo/runq.h>
#include <engine/fifo/priv.h> /* nvkm_fifo_func */
#include "TegraGM20BGpu.h"
#include "TegraGM20BHost.h"
#include "TegraGM20BRecover.h"

/* PFIFO_ENG_TIMEOUT (nvgpu hw_fifo_gm20b.h fifo_eng_timeout_r): nouveau leaves it as reset, and with it off a context
 * switch that never completes stalls every channel with no interrupt at all. Its period counts PTIMER microseconds, and
 * PTIMER runs at 19.2/31.25 of real time on this Tegra (gpu.c "stats"; nvgpu scales it the same way, scale_ptimer). The
 * period is a detection tick, not a limit: a graphics context saves only at a wait-for-idle, so a switch waits for the
 * work in flight, and the hardware raises SCHED_ERROR CTXSW_TIMEOUT again each period the switch stays pending (nvgpu
 * gk20a_fifo_check_ch_ctxsw_timeout adds up these ticks). A tick recovers the context only once its channel is stuck by
 * gpu.c's rule (tgm_ctxsw_stuck: nothing completed for TGM_STUCK_NS), so a long but finite drain, as backboardd's m2v
 * passes of 144 ms, two a frame, is waited out. */
#define ENG_TIMEOUT 0x002a0c
#define ENG_TIMEOUT_DETECTION 0x80000000
#define CTXSW_TIMEOUT_US 250000
#define PTIMER_US(us) ((us) * 1920 / 3125)
/* written 1 to clear (gk104_runl_fault_clear); a runlist's bit stops its scheduling */
#define ERROR_SCHED_DISABLE 0x00262c
#define PFIFO_INTR_0 0x002100
#define PFIFO_INTR_0_MMU_FAULT 0x10000000

static struct {
	struct nvkm_fifo_func func; /* nvkm's, the context-switch timeout replaced */
	void (*nvkm)(struct nvkm_fifo *, u32 engm); /* gf100_fifo_intr_ctxsw_timeout */
	struct nvkm_fifo *fifo;
	void *work;
	u32 engm; /* engines whose switch timed out since the work last ran (atomic) */
	u32 waited, recovered;
} ctxsw;

struct nvkm_fifo_func *tgm_fifo_func(struct nvkm_fifo *fifo)
{
	if (fifo->func != &ctxsw.func) {
		ctxsw.func = *fifo->func;
		fifo->func = &ctxsw.func;
	}
	return &ctxsw.func;
}

/* From nvkm's interrupt pass, which holds its interrupt spinlock: gpu.c's rule needs its channel list's mutex, so the
 * decision runs in ctxsw_work. */
static void ctxsw_tick(struct nvkm_fifo *fifo, u32 engm)
{
	__atomic_fetch_or(&ctxsw.engm, engm, __ATOMIC_RELAXED);
	nvxnu_work_schedule(ctxsw.work);
}

/* A runlist the hardware stopped for the tick itself (nouveau expects that only of the MMU fault a recovery triggers) is
 * let go again once the switch is waited out; one under recovery is nvkm's to clear. */
static void sched_disable_clear(struct nvkm_device *device, u32 runlm)
{
	struct nvkm_runl *runl;
	u32 set = nvkm_rd32(device, ERROR_SCHED_DISABLE) & runlm;
	static int logged;

	if (!set || (nvkm_rd32(device, PFIFO_INTR_0) & PFIFO_INTR_0_MMU_FAULT))
		return;
	nvkm_runl_foreach(runl, device->fifo)
	{
		if (atomic_read(&runl->rc_pending))
			set &= ~BIT(runl->id);
	}
	if (!set)
		return;
	nvkm_wr32(device, ERROR_SCHED_DISABLE, set);
	if (!logged++)
		nvxnu_log("tegra-gm20b: a context-switch timeout tick stopped runlists %#x "
			  "(ERROR_SCHED_DISABLE): restarted\n",
			  set);
}

static void ctxsw_work(void *arg)
{
	struct nvkm_device *device = ctxsw.fifo->engine.subdev.device;
	u32 engm = __atomic_exchange_n(&ctxsw.engm, 0, __ATOMIC_RELAXED), stuck = 0, waited = 0;
	struct nvkm_runl *runl;
	struct nvkm_engn *engn;
	bool cgid;
	int id;

	(void)arg;
	nvkm_runl_foreach(runl, ctxsw.fifo)
	{
		nvkm_runl_foreach_engn_cond(engn, runl, engm & BIT(engn->id))
		{
			if (!engn->func->chsw || !engn->func->chsw(engn))
				continue; /* the switch completed meanwhile */
			id = engn->func->cxid(engn, &cgid);
			if (id < 0)
				continue;
			if (cgid || tgm_ctxsw_stuck(id))
				stuck |= BIT(engn->id);
			else
				waited |= BIT(runl->id);
		}
	}
	if (stuck) {
		if (TGM_STATS)
			__atomic_fetch_add(&ctxsw.recovered, 1, __ATOMIC_RELAXED);
		ctxsw.nvkm(ctxsw.fifo, stuck);
	}
	if (waited) {
		if (TGM_STATS)
			__atomic_fetch_add(&ctxsw.waited, 1, __ATOMIC_RELAXED);
		sched_disable_clear(device, waited);
	}
}

void tgm_recover_init(struct nvkm_device *device)
{
	struct nvkm_fifo_func *func = tgm_fifo_func(device->fifo);

	if (ctxsw.work || !func->intr_ctxsw_timeout)
		return;
	ctxsw.fifo = device->fifo;
	ctxsw.work = nvxnu_work_alloc(ctxsw_work, NULL);
	if (!ctxsw.work)
		return;
	ctxsw.nvkm = func->intr_ctxsw_timeout;
	func->intr_ctxsw_timeout = ctxsw_tick;
}

void tgm_recover_arm(struct nvkm_device *device)
{
	nvkm_wr32(device, ENG_TIMEOUT, ENG_TIMEOUT_DETECTION | PTIMER_US(CTXSW_TIMEOUT_US));
}

void tgm_recover_fault(struct nvkm_device *device, int chid)
{
	struct nvkm_runl *runl;
	struct nvkm_engn *engn;
	u32 engm = 0;
	bool cgid;

	nvkm_runl_foreach(runl, device->fifo)
	{
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			if ((nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)) & PFIFO_ENG_BUSY) &&
			    engn->func->cxid(engn, &cgid) == chid && !cgid)
				engm |= BIT(engn->id);
		}
	}
	if (engm && ctxsw.nvkm)
		ctxsw.nvkm(device->fifo, engm);
}

void tgm_recover_stats(char *buf, unsigned int len)
{
	scnprintf(buf, len, "%u context-switch timeout ticks waited out, %u recovered",
		  __atomic_exchange_n(&ctxsw.waited, 0, __ATOMIC_RELAXED),
		  __atomic_exchange_n(&ctxsw.recovered, 0, __ATOMIC_RELAXED));
}

static int add(int *chids, int n, int max, int chid)
{
	int i;

	for (i = 0; i < n; i++) {
		if (chids[i] == chid)
			return n;
	}
	if (n < max)
		chids[n++] = chid;
	return n;
}

int tgm_recover_suspects(struct nvkm_device *device, int *chids, int max)
{
	struct nvkm_runl *runl;
	struct nvkm_engn *engn;
	bool cgid;
	int n = 0, id, i;

	nvkm_runl_foreach(runl, device->fifo)
	{
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			if (!(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)) & PFIFO_ENG_BUSY))
				continue;
			id = engn->func->cxid(engn, &cgid);
			if (id >= 0 && !cgid)
				n = add(chids, n, max, id);
		}
	}
	nvkm_runl_foreach(runl, device->fifo)
	{
		for (i = 0; i < runl->runq_nr; i++) {
			if (!runl->runq[i] ||
			    !(nvkm_rd32(device, PBDMA_STATUS(runl->runq[i]->id)) & PBDMA_BUSY))
				continue;
			n = add(chids, n, max,
				PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runl->runq[i]->id))));
		}
	}
	return n;
}
