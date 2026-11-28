/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#include <nvif/chan.h>

static void
nvif_chan_gpfifo_push_kick(struct nvif_push *push)
{
	struct nvif_chan *chan = container_of(push, typeof(*chan), push);
	u32 put = push->bgn - (u32 *)chan->push.mem.object.map.ptr;
	u32 cnt;

	if (chan->func->gpfifo.post) {
		if (push->end - push->cur < chan->func->gpfifo.post_size)
			push->end = push->cur + chan->func->gpfifo.post_size;

		WARN_ON(nvif_chan_gpfifo_post(chan));
	}

	cnt = push->cur - push->bgn;

	chan->func->gpfifo.push(chan, true, chan->push.addr + (put << 2), cnt << 2, false);
	if (!chan->defer_kick)
		chan->func->gpfifo.kick(chan);
}

static int
nvif_chan_gpfifo_push_wait(struct nvif_push *push, u32 push_nr)
{
	struct nvif_chan *chan = container_of(push, typeof(*chan), push);

	return nvif_chan_gpfifo_wait(chan, 1, push_nr);
}

int
nvif_chan_gpfifo_post(struct nvif_chan *chan)
{
	const u32 *map = chan->push.mem.object.map.ptr;
	const u32 pbptr = (chan->push.cur - map) + chan->func->gpfifo.post_size;
	const u32 gpptr = (chan->gpfifo.cur + 1) & chan->gpfifo.max;

	if (!chan->func->gpfifo.post)
		return 0;

	return chan->func->gpfifo.post(chan, gpptr, pbptr);
}

void
nvif_chan_gpfifo_push(struct nvif_chan *chan, u64 addr, u32 size, bool no_prefetch)
{
	chan->func->gpfifo.push(chan, false, addr, size, no_prefetch);
}

static int
nvif_chan_wait_step(u32 *waited_us)
{
	const u32 limit_us = 1000000;
	u32 rem;
	u32 step;

	if (*waited_us >= limit_us)
		return -ETIMEDOUT;
	rem = limit_us - *waited_us;
	if (*waited_us < 16) {
		udelay(1);
		*waited_us += 1;
		return 0;
	}
	if (!nvxnu_can_sleep()) {
		step = rem < 20 ? rem : 20;
		udelay(step);
		*waited_us += step;
		return 0;
	}
	if (*waited_us < 200)
		step = 25;
	else if (*waited_us < 2000)
		step = 100;
	else if (*waited_us < 20000)
		step = 250;
	else
		step = 1000;
	if (step > rem)
		step = rem;
	if (step < 20)
		udelay(step);
	else
		usleep_range(step, step * 2 <= rem ? step * 2 : rem);
	*waited_us += step;
	return 0;
}

int
nvif_chan_gpfifo_wait(struct nvif_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	struct nvif_push *push = &chan->push;
	int ret = 0;
	u32 waited_us = 0;

	if (gpfifo_nr) {
		/* Account for pushbuf space needed by nvif_chan_gpfifo_post(),
		 * if used after pushing userspace GPFIFO entries.
		 */
		if (chan->func->gpfifo.post)
			push_nr += chan->func->gpfifo.post_size;
	}

	/* Account for the GPFIFO entry needed to submit pushbuf. */
	if (push_nr)
		gpfifo_nr++;

	/* Wait for space in main push buffer. */
	if (push->cur + push_nr > push->end) {
		ret = nvif_chan_dma_wait(chan, push_nr);
		if (ret)
			return ret;
	}

	/* Wait for GPFIFO space. */
	while (chan->gpfifo.free < gpfifo_nr) {
		chan->gpfifo.free = chan->func->gpfifo.read_get(chan) - chan->gpfifo.cur - 1;
		if (chan->gpfifo.free < 0)
			chan->gpfifo.free += chan->gpfifo.max + 1;

		if (chan->gpfifo.free < gpfifo_nr) {
			ret = nvif_chan_wait_step(&waited_us);
			if (ret)
				return ret;
		}
	}

	return 0;
}

void
nvif_chan_gpfifo_ctor(const struct nvif_chan_func *func, void *userd, void *gpfifo, u32 gpfifo_size,
		      void *push, u64 push_addr, u32 push_size, struct nvif_chan *chan)
{
	chan->func = func;

	chan->userd.map.ptr = userd;

	chan->gpfifo.map.ptr = gpfifo;
	chan->gpfifo.max = (gpfifo_size >> 3) - 1;
	chan->gpfifo.free = chan->gpfifo.max;

	chan->push.mem.object.map.ptr = push;
	chan->push.wait = nvif_chan_gpfifo_push_wait;
	chan->push.kick = nvif_chan_gpfifo_push_kick;
	chan->push.addr = push_addr;
	chan->push.hw.max = push_size >> 2;
	chan->push.bgn = chan->push.cur = chan->push.end = push;
}

int
nvif_chan_dma_wait(struct nvif_chan *chan, u32 nr)
{
	struct nvif_push *push = &chan->push;
	u32 cur = push->cur - (u32 *)push->mem.object.map.ptr;
	u32 free, waited_us = 0;
	int ret;

	nr += chan->func->gpfifo.post_size;

	do {
		u32 get = chan->func->push.read_get(chan);

		if (get <= cur) {
			free = push->hw.max - cur;
			if (free >= nr)
				break;

			PUSH_KICK(push);

			while (get == 0) {
				get = chan->func->push.read_get(chan);
				if (get == 0) {
					ret = nvif_chan_wait_step(&waited_us);
					if (ret)
						return ret;
				}
			}

			cur = 0;
		}

		free = get - cur - 1;

		if (free < nr) {
			ret = nvif_chan_wait_step(&waited_us);
			if (ret)
				return ret;
		}
	} while (free < nr);

	push->bgn = (u32 *)push->mem.object.map.ptr + cur;
	push->cur = push->bgn;
	push->end = push->bgn + free - chan->func->gpfifo.post_size;
	return 0;
}
