// SPDX-License-Identifier: MIT
/* See TegraGM20BRecover.h. */
#include <core/device.h>
#include <engine/fifo.h>
#include <engine/fifo/runl.h>
#include <engine/fifo/runq.h>
#include <engine/fifo/chan.h> /* nvkm_chan_func: the unbind */
#include <engine/fifo/cgrp.h>
#include <engine/fifo/priv.h> /* nvkm_fifo_func */
#include <core/engine.h>      /* nvkm_engine_reset */
#include "TegraGM20BGpu.h"
#include "TegraGM20BHost.h"
#include "TegraGM20BRecover.h"

/* PFIFO_ENG_TIMEOUT (nvgpu hw_fifo_gm20b.h fifo_eng_timeout_r): nouveau leaves it as reset, and with it off a context
 * switch that never completes stalls every channel with no interrupt at all. Its period counts PTIMER microseconds, and
 * PTIMER runs at 19.2/31.25 of real time on this Tegra (gpu_stats.c "stats"; nvgpu scales it the same way, scale_ptimer),
 * and its period is nvgpu's (GRFIFO_TIMEOUT_CHECK_PERIOD_US). The period is a detection tick, not a limit: a graphics
 * context saves only at a wait-for-idle, so a switch waits for the work in flight, and the hardware raises SCHED_ERROR
 * CTXSW_TIMEOUT again each period the switch stays pending (nvgpu gk20a_fifo_check_ch_ctxsw_timeout adds up these ticks).
 * A tick recovers the context only once its channel is stuck by gpu_hang.c's rule (tgm_ctxsw_decide): at the first tick
 * when it has nothing pending or the engine is a copy engine, after TGM_STUCK_NS with nothing completed on GR, so a long
 * but finite GR drain is waited out. */
#define ENG_TIMEOUT 0x002a0c
#define ENG_TIMEOUT_DETECTION 0x80000000
#define CTXSW_TIMEOUT_US 100000
#define PTIMER_US(us) ((us) * 1920u / 3125u)
#define PFIFO_INTR_0 0x002100
#define PFIFO_INTR_0_BIND_ERROR 0x00000001
#define PFIFO_INTR_0_SCHED_ERROR 0x00000100
#define PFIFO_INTR_0_MMU_FAULT 0x10000000
#define PFIFO_INTR_EN_0 0x002140
#define PFIFO_INTR_BIND_ERROR 0x00252c
#define PFIFO_INTR_SCHED_ERROR 0x00254c

static struct {
	struct nvkm_fifo_func func; /* nvkm's, the context-switch timeout replaced */
	void (*nvkm)(struct nvkm_fifo *, u32 engm); /* gf100_fifo_intr_ctxsw_timeout */
	nvkm_inth_func fifo_intr;                   /* gk104_fifo_intr */
	const struct nvkm_runq_func *runq_nvkm;
	struct nvkm_runq_func runq; /* nvkm's, the interrupt handler wrapped */
	const struct nvkm_chan_func *chan_nvkm;
	struct nvkm_chan_func chan; /* nvkm's, the unbind wrapped */
	struct nvkm_fifo *fifo;
	void *work;
	u32 engm;   /* engines whose switch timed out since the work last ran (atomic) */
	u32 resume; /* a scheduler error came since the work last ran (atomic) */
	u32 waited, recovered, redone;
	struct nvkm_engn_func engn, engn_ce; /* nvkm's, the context decode replaced (engn_cxid) */
} ctxsw;

struct nvkm_fifo_func *tgm_fifo_func(struct nvkm_fifo *fifo)
{
	if (fifo->func != &ctxsw.func) {
		ctxsw.func = *fifo->func;
		fifo->func = &ctxsw.func;
	}
	return &ctxsw.func;
}

/* From nvkm's interrupt pass, which holds its interrupt spinlock: gpu_hang.c's rule needs its channel list's mutex, so the
 * decision runs in ctxsw_work. */
static void ctxsw_tick(struct nvkm_fifo *fifo, u32 engm)
{
	__atomic_fetch_or(&ctxsw.engm, engm, __ATOMIC_RELAXED);
	__atomic_store_n(&ctxsw.resume, 1, __ATOMIC_RELAXED);
	nvxnu_work_schedule(ctxsw.work);
}

static u32 engines_switching(struct nvkm_fifo *fifo)
{
	struct nvkm_device *device = fifo->engine.subdev.device;
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	u32 engm = 0;

	nvkm_runl_foreach(runl, fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid && engn->fault >= 0)
		{
			if (tgm_engine_switch_pending(
				    nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id))))
				engm |= BIT((u32)engn->id);
		}
	}
	return engm;
}

/* nvkm's engine context decode replaced by NVIDIA's (tgm_engine_chid): nvkm_runl_rc resets an engine for a channel
 * group only while that group's context is on it, never because it was the last one before a load. */
static int engn_cxid(struct nvkm_engn *engn, bool *cgid)
{
	struct nvkm_device *device = engn->runl->fifo->engine.subdev.device;
	const u32 status = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
	const int switching = PFIFO_ENG_CTX_STATUS(status) == PFIFO_ENG_CTX_SWITCH;
	int tsg = 0;
	const int id =
		tgm_engine_chid(status, switching && nvkm_engine_chsw_load(engn->engine), &tsg);

	*cgid = tsg != 0;
	return id < 0 ? -ENODEV : id;
}

static const struct nvkm_engn_func *engn_func(struct nvkm_engn_func *copy,
					      const struct nvkm_engn_func *nvkm)
{
	if (!nvkm || !nvkm->cxid)
		return nvkm;
	*copy = *nvkm;
	copy->cxid = engn_cxid;
	return copy;
}

/* Every SCHED_ERROR code is handled as nvgpu's gk20a_fifo_handle_sched_error handles it: the engines busy mid-switch
 * get a context-switch timeout's decision, and with none there is nothing to recover. nvkm's own handler names only
 * CTXSW_TIMEOUT and does nothing for the rest, and a report naming no channel would be fatal to gpu_hang.c. */
static void sched_error(struct nvkm_device *device)
{
	u32 code = nvkm_rd32(device, PFIFO_INTR_SCHED_ERROR) & 0xff;
	u32 engm = engines_switching(ctxsw.fifo);

	static u32 unexplained;

	if (code != TGM_SCHED_CTXSW_TIMEOUT || (!engm && unexplained++ < 4)) {
		nvxnu_log("tegra-gm20b: GPU scheduler error %#04x (%s), engines mid-switch %#x; "
			  "ENG0 %08x ENG1 %08x, PBDMA0 %08x ch %u\n",
			  code, tgm_sched_error_name(code), engm,
			  nvkm_rd32(device, PFIFO_ENG_STATUS(0)),
			  nvkm_rd32(device, PFIFO_ENG_STATUS(1)),
			  nvkm_rd32(device, PBDMA_STATUS(0)),
			  PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(0))));
	}
	ctxsw_tick(ctxsw.fifo, engm);
}

/* nvgpu's gk20a_fifo_intr counts a bind error as no channel's: it is logged. nvkm reports it, naming no channel. */
static void bind_error(struct nvkm_device *device)
{
	u32 code = nvkm_rd32(device, PFIFO_INTR_BIND_ERROR) & 0xff;

	nvxnu_log("tegra-gm20b: GPU bind error %#04x (%s)\n", code, tgm_bind_error_name(code));
}

static irqreturn_t fifo_intr(struct nvkm_inth *inth)
{
	struct nvkm_device *device = ctxsw.fifo->engine.subdev.device;
	u32 stat = nvkm_rd32(device, PFIFO_INTR_0) & nvkm_rd32(device, PFIFO_INTR_EN_0);

	if (stat & PFIFO_INTR_0_BIND_ERROR) {
		bind_error(device);
		nvkm_wr32(device, PFIFO_INTR_0, PFIFO_INTR_0_BIND_ERROR);
	}
	if (stat & PFIFO_INTR_0_SCHED_ERROR) {
		sched_error(device);
		nvkm_wr32(device, PFIFO_INTR_0, PFIFO_INTR_0_SCHED_ERROR);
	}
	return ctxsw.fifo_intr(inth);
}

/* nvkm's PBDMA handler writes a NOP over the pending method for any interrupt, so it runs only for the bits left. */
static bool runq_intr(struct nvkm_runq *runq, struct nvkm_runl *runl)
{
	struct nvkm_device *device = runq->fifo->engine.subdev.device;
	u32 intr0 = nvkm_rd32(device, PBDMA_INTR_0(runq->id)) &
		    nvkm_rd32(device, PBDMA_INTR_EN_0(runq->id));
	u32 intr1 = nvkm_rd32(device, PBDMA_INTR_1(runq->id)) &
		    nvkm_rd32(device, PBDMA_INTR_EN_1(runq->id));

	if (intr0 & TGM_PBDMA_INTR_ACQUIRE) {
		tgm_acquire_intr(device, runq->id);
		nvkm_wr32(device, PBDMA_INTR_0(runq->id), TGM_PBDMA_INTR_ACQUIRE);
	}
	if (intr0 & TGM_PBDMA_ENG_RESET)
		tgm_eng_reset_intr(device, runq);
	if (!tgm_pbdma_for_nvkm(intr0) && !intr1)
		return true;
	return ctxsw.runq_nvkm->intr(runq, runl);
}

/* A runlist a scheduler error stopped (the hardware stops it for a context-switch timeout tick too; nouveau expects that
 * only of the MMU fault a recovery triggers) is let go again; one under recovery is nvkm's to clear. */
static void sched_disable_clear(struct nvkm_device *device, u32 runlm)
{
	struct nvkm_runl *runl = NULL;
	u32 set = nvkm_rd32(device, ERROR_SCHED_DISABLE) & runlm;
	static int logged;

	if (!set || (nvkm_rd32(device, PFIFO_INTR_0) & PFIFO_INTR_0_MMU_FAULT))
		return;
	nvkm_runl_foreach(runl, device->fifo)
	{
		if (atomic_read(&runl->rc_pending))
			set &= ~BIT((u32)runl->id);
	}
	if (!set)
		return;
	nvkm_wr32(device, ERROR_SCHED_DISABLE, set);
	if (!logged++) {
		nvxnu_log("tegra-gm20b: a scheduler error stopped runlists %#x "
			  "(ERROR_SCHED_DISABLE): restarted\n",
			  set);
	}
}

/* nvkm unbinds a channel after its preempt even when the preempt timed out (nvkm_chan_remove does not look at
 * nvkm_runl_preempt_wait's answer), and an engine still on the channel then raises BIND_ERROR UNBIND_WHILE_RUNNING. As
 * nvgpu's teardown recovers a channel its preempt could not take off an engine, that engine is reset before the unbind. */
static void chan_unbind(struct nvkm_chan *chan)
{
	struct nvkm_runl *runl = chan->cgrp->runl;
	struct nvkm_device *device = runl->fifo->engine.subdev.device;
	struct nvkm_engn *engn = NULL;

	mutex_lock(&runl->mutex);
	/* nouveau's runlist walk takes each engine by container_of. */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
	{
		if (tgm_engine_holds(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)), chan->id))
			tgm_engine_reset_locked(runl, engn, chan->id, "still on unbound");
	}
	mutex_unlock(&runl->mutex);
	ctxsw.chan_nvkm->unbind(chan);
}

#if TGM_STATS
int tgm_recover_copy_reset(int chid, void (*mark)(void), uint32_t *status)
{
	struct nvkm_device *device = ctxsw.fifo->engine.subdev.device;
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	int reset = 0;

	nvkm_runl_foreach(runl, ctxsw.fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(
			engn, runl, engn->func->cxid && engn->engine->subdev.type == NVKM_ENGINE_CE)
		{
			*status = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
			if (!tgm_engine_holds(*status, chid))
				continue;
			mark();
			mutex_lock(&runl->mutex);
			if (tgm_engine_holds(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)), chid)) {
				tgm_engine_reset_locked(runl, engn, chid,
							"busy (HW Test) with kernel");
				__atomic_fetch_add(&ctxsw.redone, 1, __ATOMIC_RELAXED);
				reset = 1;
			}
			mutex_unlock(&runl->mutex);
		}
	}
	return reset;
}
#endif

static void ctxsw_work(void *arg)
{
	struct nvkm_device *device = ctxsw.fifo->engine.subdev.device;
	u32 engm = __atomic_exchange_n(&ctxsw.engm, 0, __ATOMIC_RELAXED);
	u32 resume = __atomic_exchange_n(&ctxsw.resume, 0, __ATOMIC_RELAXED) ? ~0u : 0;
	u32 stuck = 0;
	int waited = 0;
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	enum tgm_ctxsw verdict = TGM_CTXSW_WAIT;
	bool cgid = false;
	u32 status = 0;
	int id = 0;

	(void)arg;
	nvkm_runl_foreach(runl, ctxsw.fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl,
					    engn->func->cxid && (engm & BIT((u32)engn->id)))
		{
			status = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
			if (!tgm_engine_switch_pending(status))
				continue; /* the switch completed meanwhile */
			id = engn->func->cxid(engn, &cgid);
			if (id < 0)
				continue;
			if (cgid)
				verdict = TGM_CTXSW_KILL;
			else if (tgm_engine_load_stuck(status))
				verdict = tgm_ctxsw_load_decide(id, engn->engine->subdev.name);
			else
				verdict = tgm_ctxsw_decide(id, engn->engine->subdev.name,
							   engn->engine->subdev.type ==
								   NVKM_ENGINE_GR);
			switch (verdict) {
			case TGM_CTXSW_KILL:
				stuck |= BIT((u32)engn->id);
				resume &= ~BIT((u32)runl->id);
				break;
			case TGM_CTXSW_RESET:
			case TGM_CTXSW_REDO:
				mutex_lock(&runl->mutex);
				if (tgm_engine_holds(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)),
						     id)) {
					tgm_engine_reset_locked(
						runl, engn, id,
						verdict == TGM_CTXSW_REDO
							? "stuck switching the context of kernel"
							: "stuck switching the context of killed, "
							  "unknown or idle kernel");
				}
				mutex_unlock(&runl->mutex);
				if (verdict == TGM_CTXSW_REDO && TGM_STATS)
					__atomic_fetch_add(&ctxsw.redone, 1, __ATOMIC_RELAXED);
				break;
			case TGM_CTXSW_RELOAD:
				mutex_lock(&runl->mutex);
				status = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
				if (tgm_engine_load_stuck(status) && tgm_engine_holds(status, id)) {
					tgm_engine_reset_locked(
						runl, engn, id,
						"stuck loading the context of live");
				}
				mutex_unlock(&runl->mutex);
				break;
			case TGM_CTXSW_WAIT:
			case TGM_CTXSW_FATAL:
				waited = 1;
				break;
			}
		}
	}
	if (stuck) {
		if (TGM_STATS)
			__atomic_fetch_add(&ctxsw.recovered, 1, __ATOMIC_RELAXED);
		ctxsw.nvkm(ctxsw.fifo, stuck);
	}
	if (waited && TGM_STATS)
		__atomic_fetch_add(&ctxsw.waited, 1, __ATOMIC_RELAXED);
	if (resume)
		sched_disable_clear(device, resume);
}

/* At M2 the FIFO has no user yet, so nvkm makes its runqueues and registers its interrupt handler later, at the first
 * channel (nvkm_fifo_oneinit), from its function table: recover.c's entries go in that table, and onto what exists already. */
void tgm_recover_init(struct nvkm_device *device)
{
	struct nvkm_fifo_func *func = tgm_fifo_func(device->fifo);
	struct nvkm_inth *inth = &device->fifo->engine.subdev.inth;
	const struct nvkm_engn_func *gr = NULL, *ce = NULL;
	struct nvkm_runq *runq = NULL;
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;

	if (ctxsw.work)
		return;
	if (!func->intr_ctxsw_timeout || !func->intr || !func->runq || !func->chan.func ||
	    !func->chan.func->unbind) {
		nvxnu_log("tegra-gm20b: nvkm's FIFO lacks what recover.c takes over: scheduler "
			  "errors, "
			  "context-switch timeouts and PBDMA ENG_RESET stay nvkm's\n");
		return;
	}
	ctxsw.fifo = device->fifo;
	ctxsw.work = nvxnu_work_alloc(ctxsw_work, NULL);
	if (!ctxsw.work)
		return;
	tgm_acquire_init();
	ctxsw.nvkm = func->intr_ctxsw_timeout;
	func->intr_ctxsw_timeout = ctxsw_tick;
	ctxsw.fifo_intr = func->intr;
	func->intr = fifo_intr;
	if (inth->func == ctxsw.fifo_intr)
		inth->func = fifo_intr;
	ctxsw.runq_nvkm = func->runq;
	ctxsw.runq = *func->runq;
	ctxsw.runq.intr = runq_intr;
	func->runq = &ctxsw.runq;
	nvkm_runq_foreach(runq, device->fifo)
	{
		if (runq->func == ctxsw.runq_nvkm)
			runq->func = &ctxsw.runq;
	}
	ctxsw.chan_nvkm = func->chan.func;
	ctxsw.chan = *func->chan.func;
	ctxsw.chan.unbind = chan_unbind;
	func->chan.func = &ctxsw.chan;
	gr = func->engn;
	ce = func->engn_ce;
	func->engn = engn_func(&ctxsw.engn, gr);
	func->engn_ce = engn_func(&ctxsw.engn_ce, ce);
	nvkm_runl_foreach(runl, device->fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn(engn, runl)
		{
			if (engn->func == gr)
				engn->func = func->engn;
			else if (engn->func == ce)
				engn->func = func->engn_ce;
		}
	}
}

void tgm_recover_suspend(void)
{
	tgm_promise_suspend();
	if (!ctxsw.work)
		return;
	nvxnu_work_cancel_wait(ctxsw.work);
	__atomic_store_n(&ctxsw.engm, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ctxsw.resume, 0, __ATOMIC_RELAXED);
	tgm_acquire_suspend();
}

void tgm_recover_arm(struct nvkm_device *device)
{
	nvkm_wr32(device, ENG_TIMEOUT, ENG_TIMEOUT_DETECTION | PTIMER_US(CTXSW_TIMEOUT_US));
}

void tgm_recover_fault(struct nvkm_device *device, int chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	u32 engm = 0;
	bool cgid = false;

	nvkm_runl_foreach(runl, device->fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			if ((nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)) & PFIFO_ENG_BUSY) &&
			    engn->func->cxid(engn, &cgid) == chid && !cgid)
				engm |= BIT((u32)engn->id);
		}
	}
	if (engm && ctxsw.nvkm)
		ctxsw.nvkm(device->fifo, engm);
}

void tgm_recover_stats(char *buf, unsigned int len)
{
	scnprintf(buf, len,
		  "%u context-switch timeout ticks waited out, %u recovered, %u engines reset, "
		  "%u kernel works redone",
		  __atomic_exchange_n(&ctxsw.waited, 0, __ATOMIC_RELAXED),
		  __atomic_exchange_n(&ctxsw.recovered, 0, __ATOMIC_RELAXED),
		  tgm_engine_resets_take(),
		  __atomic_exchange_n(&ctxsw.redone, 0, __ATOMIC_RELAXED));
}

static int add(int *chids, int n, int max, int chid)
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

int tgm_recover_suspects(struct nvkm_device *device, int *chids, int max)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	bool cgid = false;
	int n = 0;
	int id = 0;
	int i = 0;

	nvkm_runl_foreach(runl, device->fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			if (!(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)) &
			      (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW)))
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
			    !(nvkm_rd32(device, PBDMA_STATUS(runl->runq[i]->id)) & PBDMA_BUSY) ||
			    tgm_pbdma_acquiring(nvkm_rd32(device, PBDMA_METHOD0(runl->runq[i]->id)),
						nvkm_rd32(device, PBDMA_DATA0(runl->runq[i]->id))))
				continue;
			n = add(chids, n, max,
				PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runl->runq[i]->id))));
		}
	}
	return n;
}
