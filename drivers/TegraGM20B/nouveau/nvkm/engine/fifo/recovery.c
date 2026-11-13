/*
 * Copyright 2021 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include <nouveau/nvkm/engine/fifo/runl.h>
#include <nouveau/nvkm/engine/fifo/cgrp.h>
#include <nouveau/nvkm/engine/fifo/chan.h>
#include <nouveau/nvkm/engine/fifo/chid.h>
#include <nouveau/nvkm/engine/fifo/priv.h>
#include <nouveau/nvkm/engine/fifo/runq.h>
#include <subdev/timer.h>

void tgm_recover_reset_hold(struct nvkm_engine *);
void tgm_recover_reset_release(struct nvkm_engine *);
void tgm_recover_preempt_unblock(struct nvkm_runl *);

struct nvkm_cgrp *
nvkm_engn_cgrp_get(struct nvkm_engn *engn, unsigned long *pirqflags)
{
	struct nvkm_cgrp *cgrp = NULL;
	struct nvkm_chan *chan;
	bool cgid;
	int id;

	id = engn->func->cxid(engn, &cgid);
	if (id < 0)
		return NULL;

	if (!cgid) {
		chan = nvkm_runl_chan_get_chid(engn->runl, id, pirqflags);
		if (chan)
			cgrp = chan->cgrp;
	} else {
		cgrp = nvkm_runl_cgrp_get_cgid(engn->runl, id, pirqflags);
	}

	WARN_ON(!cgrp);
	return cgrp;
}

void
nvkm_runl_rc(struct nvkm_runl *runl)
{
	struct nvkm_fifo *fifo = runl->fifo;
	struct nvkm_cgrp *cgrp, *gtmp;
	struct nvkm_chan *chan, *ctmp;
	struct nvkm_engn *engn;
	unsigned long flags;
	int rc, state, i;
	bool reset;

	/* Runlist is blocked before scheduling recovery - fetch count. */
	BUG_ON(!mutex_is_locked(&runl->mutex));
	rc = atomic_xchg(&runl->rc_pending, 0);
	if (!rc)
		return;

	/* Look for channel groups flagged for RC. */
	nvkm_runl_foreach_cgrp_safe(cgrp, gtmp, runl) {
		state = atomic_cmpxchg(&cgrp->rc, NVKM_CGRP_RC_PENDING, NVKM_CGRP_RC_RUNNING);
		if (state == NVKM_CGRP_RC_PENDING) {
			/* Disable all channels in them, and remove from runlist. */
			nvkm_cgrp_foreach_chan_safe(chan, ctmp, cgrp) {
				nvkm_chan_error(chan, false);
				nvkm_chan_remove_locked(chan);
			}
		}
	}

	/* On GPUs with runlist preempt, wait for PBDMA(s) servicing runlist to go idle. */
	if (runl->func->preempt) {
		for (i = 0; i < runl->runq_nr; i++) {
			struct nvkm_runq *runq = runl->runq[i];

			if (runq) {
				BUG_ON(nvkm_msec(fifo->engine.subdev.device, 2000,
					if (runq->func->idle(runq))
						break;
				) < 0);
			}
		}
	}

	/* Look for engines that are still on flagged channel groups - reset them. */
	nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid) {
		cgrp = nvkm_engn_cgrp_get(engn, &flags);
		if (!cgrp) {
			ENGN_DEBUG(engn, "cxid not valid");
			continue;
		}

		reset = atomic_read(&cgrp->rc) == NVKM_CGRP_RC_RUNNING;
		nvkm_cgrp_put(&cgrp, flags);
		if (!reset) {
			ENGN_DEBUG(engn, "cxid not in recovery");
			continue;
		}

		ENGN_DEBUG(engn, "resetting...");
		tgm_recover_reset_hold(engn->engine);
		BUG_ON(nvkm_engine_reset(engn->engine));
		tgm_recover_reset_release(engn->engine);
	}

	/* Submit runlist update, and clear any remaining exception state. */
	BUG_ON(runl->func->update(runl));
	if (runl->func->fault_clear)
		runl->func->fault_clear(runl);

	/* Unblock runlist processing. */
	while (rc--)
		nvkm_runl_allow(runl);
	BUG_ON(runl->func->wait(runl));
	if (runl->func->preempt_pending) {
		tgm_recover_preempt_unblock(runl);
		BUG_ON(nvkm_msec(fifo->engine.subdev.device, 2000,
			if (!runl->func->preempt_pending(runl))
				break;
		) < 0);
	}

	/* Removed channels remain in chid until their destructor takes runl->mutex.
	 * ERRORED releases external memory leases, so publish only after quiescence. */
	for (i = 0; i < runl->chid->nr; i++) {
		flags = 0;
		chan = nvkm_runl_chan_get_chid(runl, i, &flags);
		if (chan && chan->cgrp->runl == runl &&
		    atomic_read(&chan->cgrp->rc) == NVKM_CGRP_RC_RUNNING &&
		    atomic_cmpxchg(&chan->errored, 1, 2) == 1)
			nvkm_event_ntfy(&runl->chid->event, chan->id, NVKM_CHAN_EVENT_ERRORED);
		nvkm_chan_put(&chan, flags);
	}
}

