// SPDX-License-Identifier: MIT
// What the host keeps of each channel's RAMFC timeslice, and of one written through the level change's path, for
// debug.switch_gpu_ramfc (TegraGM20BRamfcProbe.cpp) and HW Tests gpu.ramfc_timeslice and gpu.ramfc_creation: debug
// kernels only.
#include "gpu_internal.h"
#include <engine/fifo/chan.h>
#include "TegraGM20BGrPreempt.h"

#if TGM_DEBUG_PROBES
static void ramfc_owner(struct tgm_ramfc_look *look, const struct tgm_chan *chan, size_t *at)
{
	char owner[sizeof(chan->owner)];
	size_t i = 0;

	for (i = 0; i + 1 < sizeof(owner) && chan->owner[i]; i++)
		owner[i] = chan->owner[i] == ' ' || chan->owner[i] == ',' ? '_' : chan->owner[i];
	owner[i] = 0;
	*at += scnprintf(look->owners + *at, sizeof(look->owners) - *at, "%s%u:%s:%c",
			 *at ? "," : "", chan->chid, owner, chan->timeslice_created ? 'c' : 'l');
}

/* Under tgm_gpu_held (the GPU up: nvkm_kmap flushes the L2) and tgm_live_lock (each channel's instance block kept). Only
 * channels with a job completed since the write count: that job's load and save of RAMFC are what a write that raced
 * the host's save loses to. */
static void ramfc_look(struct nvkm_device *device, void *arg)
{
	struct tgm_ramfc_look *look = arg;
	struct tgm_chan *chan = NULL;
	size_t at = 0;

	(void)device;
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		u32 value = 0;

		look->channels++;
		if (READ_ONCE(chan->error) || READ_ONCE(chan->timeslice_due) ||
		    (s32)(tgm_chan_completed(chan) - chan->timeslice_at) <= 0)
			continue;
		value = tgm_ramfc_read(chan->chid, RAMFC_TIMESLICE);
		if (chan->kernel) {
			look->kernel_checked++;
			look->kernel_mismatched += value != chan->timeslice;
			continue;
		}
		look->checked++;
		look->high += chan->timeslice == RAMFC_TIMESLICE_HIGH;
		if (value == chan->timeslice)
			continue;
		look->created_mismatched += chan->timeslice_created != 0;
		ramfc_owner(look, chan, &at);
		if (!look->mismatched++) {
			look->chid = chan->chid;
			look->ramfc = value;
			look->expected = chan->timeslice;
		}
	}
	mutex_unlock(&tgm_live_lock);
	if (!at)
		scnprintf(look->owners, sizeof(look->owners), "-");
}

struct ramfc_word {
	u32 chid, value;
};

static void ramfc_word_write(void *arg)
{
	struct ramfc_word *word = arg;

	tgm_ramfc_write(word->chid, RAMFC_TIMESLICE, word->value);
}

static void ramfc_word_read(struct nvkm_device *device, void *arg)
{
	struct ramfc_word *word = arg;

	(void)device;
	word->value = tgm_ramfc_read(word->chid, RAMFC_TIMESLICE);
}

/* A level change's write (tgm_sched_ramfc) of `value` into the kernel probe channel's timeslice, a job on that channel
 * (its RAMFC loaded and saved by the host), and the word read back. */
static int ramfc_flip(u32 chid, u32 value, u32 *read)
{
	struct ramfc_word word = {chid, value};
	u64 us = 0;
	int ret = 0;

	tgm_gpu_lock();
	ret = tgm_sched_ramfc(tgm_gpu.device, chid, ramfc_word_write, &word, 1);
	mutex_unlock(&tgm_gpu.lock);
	if (!ret)
		ret = tgm_probe_fence(&chid, &us);
	word.value = 0;
	if (!ret)
		ret = tgm_gpu_held(ramfc_word_read, &word);
	*read = word.value;
	return ret;
}

/* The passive look over the client channels, then the probe channel's timeslice changed and changed back, each read
 * back after a job: a write the host's save of a still-resident channel lands on reads back as the old value. */
int tgm_ramfc_probe(struct tgm_ramfc_look *look)
{
	struct ramfc_word from = {0, 0};
	u64 us = 0;
	int ret = 0;

	*look = (struct tgm_ramfc_look){0};
	ret = tgm_gpu_held(ramfc_look, look);
	if (!ret)
		ret = tgm_probe_fence(&from.chid, &us);
	if (!ret)
		ret = tgm_gpu_held(ramfc_word_read, &from);
	if (ret) {
		look->flip_ret = ret;
		return ret == -EBUSY ? ret : 0;
	}
	look->flip_chid = from.chid;
	look->flip_from = from.value;
	look->flip_to =
		from.value == RAMFC_TIMESLICE_HIGH ? RAMFC_TIMESLICE_DEFAULT : RAMFC_TIMESLICE_HIGH;
	look->flip_ret = ramfc_flip(from.chid, look->flip_to, &look->flip_read);
	if (!look->flip_ret)
		look->flip_ret = ramfc_flip(from.chid, from.value, &look->back_read);
	return 0;
}

static struct nvkm_gpuobj *chan_gr_ctx(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	struct nvkm_vctx *vctx = NULL;
	struct nvkm_gpuobj *inst = NULL;
	unsigned long flags = 0;

	nvkm_runl_foreach(runl, tgm_gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			if (nchan->cgrp) {
				list_for_each_entry(vctx, &nchan->cgrp->vctxs, head)
				{
					if (vctx->ectx && vctx->ectx->engn &&
					    vctx->ectx->engn->engine &&
					    vctx->ectx->engn->engine->subdev.type ==
						    NVKM_ENGINE_GR) {
						inst = vctx->inst;
						break;
					}
				}
			}
			nvkm_chan_put(&nchan, flags);
			break;
		}
	}
	return inst;
}

static uint32_t gr_ctx_read(void *arg, uint32_t offset)
{
	return nvkm_ro32((struct nvkm_gpuobj *)arg, offset);
}

static void gr_ctx_write(void *arg, uint32_t offset, uint32_t value)
{
	nvkm_wo32((struct nvkm_gpuobj *)arg, offset, value);
}

struct gr_preempt_write {
	struct nvkm_gpuobj *ctx;
	struct tgm_gr_preempt_ctx op;
};

static void gr_preempt_write(void *arg)
{
	struct gr_preempt_write *w = arg;

	nvkm_kmap(w->ctx);
	tgm_gr_preempt_write_cb(&w->op);
	nvkm_done(w->ctx);
}

int tgm_chan_gr_preempt(struct tgm_chan *chan, uint32_t mode, uint32_t *old_mode,
			uint32_t *readback)
{
	struct tgm_gr_preempt_io io = {NULL, gr_ctx_read, gr_ctx_write};
	struct gr_preempt_write w = {NULL, {NULL, &io, mode, 0, 0}};
	int ret = 0;

	if (!chan || chan->kernel || mode > TGM_GR_CTX_PREEMPT_CTA)
		return -EINVAL;
	if (READ_ONCE(chan->error))
		return -EIO;
	tgm_gpu_lock();
	w.ctx = chan_gr_ctx(chan->chid);
	if (!w.ctx || w.ctx->size < TGM_GR_CTX_PREEMPT_MIN_SIZE) {
		ret = -ENOENT;
	} else {
		io.arg = w.ctx;
		ret = tgm_sched_ramfc(tgm_gpu.device, chan->chid, gr_preempt_write, &w, 1);
	}
	mutex_unlock(&tgm_gpu.lock);
	if (!ret) {
		if (old_mode)
			*old_mode = w.op.old_mode;
		if (readback)
			*readback = w.op.readback;
	}
	return ret;
}
#endif
