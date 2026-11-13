/* SPDX-License-Identifier: MIT */
void switch_gpu_events_completed(void) __attribute__((weak));

static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)
{
	struct tgm_chan *chan = container_of(event, struct tgm_chan, nonstall);
	u32 done = tgm_chan_completed(chan);
	u64 now = 0;
	(void)repv;
	(void)repc;
#if TGM_STATS
	if (chan == tgm_present.chan)
		tgm_copy_reset_nonstall();
#endif
	/* a mid-job event release raises the interrupt without moving the fence (SwitchGpuEventWord.h) */
	if ((s32)(READ_ONCE(chan->event_through) - done) > 0 && switch_gpu_events_completed)
		switch_gpu_events_completed();
	if (done == chan->progress_done)
		return switch_gpu_irq_complete(&chan->irq_state, done) ? NVIF_EVENT_DROP
								       : NVIF_EVENT_KEEP;
	if (__atomic_load_n(&chan->high, __ATOMIC_ACQUIRE))
		high_fenced(chan, chan->progress_done, done);
	chan->progress_done = done;
	now = nvxnu_nanotime();
	WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, now));
	WRITE_ONCE(chan->progress_ns, now);
	idle_fenced(chan);
	if (chan->kernel == CHAN_SCRUB) {
		scrub_fenced(chan);
		/* nobody sleeps on its fence but system sleep's drain and the wake's test fence */
		if (tgm_gate_is_shut(&tgm_sc7_gate))
			nvxnu_wake();
		return switch_gpu_irq_complete(&chan->irq_state, done) ? NVIF_EVENT_DROP
								       : NVIF_EVENT_KEEP;
	}
	if (stats_on())
		stats_fences(chan);
	if (chan->kernel) {
		present_fenced(chan);
	} else {
		chan_wake_waiters(chan);
		present_watch_fenced(chan, 0);
	}
	if (READ_ONCE(chan->fenced))
		chan->fenced(chan->fenced_arg);
	return switch_gpu_irq_complete(&chan->irq_state, done) ? NVIF_EVENT_DROP : NVIF_EVENT_KEEP;
}
