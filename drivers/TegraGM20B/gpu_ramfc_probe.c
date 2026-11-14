// SPDX-License-Identifier: MIT
// What the host keeps of each channel's RAMFC timeslice, and of one written through the level change's path, for
// debug.switch_gpu_ramfc (TegraGM20BRamfcProbe.cpp) and HW Test gpu.ramfc_timeslice: debug kernels only.
#include "gpu_internal.h"

#if TGM_STATS
/* Under tgm_gpu_held (the GPU up: nvkm_kmap flushes the L2) and tgm_live_lock (each channel's instance block kept). Only
 * channels with a job completed since the write count: that job's load and save of RAMFC are what a write that raced
 * the host's save loses to. */
static void ramfc_look(struct nvkm_device *device, void *arg)
{
	struct tgm_ramfc_look *look = arg;
	struct tgm_chan *chan = NULL;

	(void)device;
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		u32 value = 0;

		look->channels++;
		if (chan->kernel || READ_ONCE(chan->error) || READ_ONCE(chan->timeslice_due) ||
		    (s32)(tgm_chan_completed(chan) - chan->timeslice_at) <= 0)
			continue;
		value = tgm_ramfc_read(chan->chid, RAMFC_TIMESLICE);
		look->checked++;
		look->high += chan->timeslice == RAMFC_TIMESLICE_HIGH;
		if (value != chan->timeslice && !look->mismatched++) {
			look->chid = chan->chid;
			look->ramfc = value;
			look->expected = chan->timeslice;
		}
	}
	mutex_unlock(&tgm_live_lock);
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
#endif
