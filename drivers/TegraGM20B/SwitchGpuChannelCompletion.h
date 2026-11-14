/* SPDX-License-Identifier: MIT */
void switch_gpu_events_completed(void) __attribute__((weak));

/* The interrupt pass, for a channel tgm_busy showed with work in flight: 1 when its fence moved */
static int chan_completion(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);
	u64 now = 0;
#if TGM_STATS
	if (chan == tgm_present.chan)
		tgm_copy_reset_nonstall();
#endif
	/* a mid-job event release raises the interrupt without moving the fence (SwitchGpuEventWord.h) */
	if ((s32)(READ_ONCE(chan->event_through) - done) > 0 && switch_gpu_events_completed)
		switch_gpu_events_completed();
	if (done == chan->progress_done)
		return 0;
	if (__atomic_load_n(&chan->high, __ATOMIC_ACQUIRE))
		high_fenced(chan, chan->progress_done, done);
	chan->progress_done = done;
	switch_gpu_busy_done(&tgm_busy, chan->chid, done);
	now = nvxnu_nanotime();
	WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, now));
	WRITE_ONCE(chan->progress_ns, now);
	chan_wake_waiters(chan);
	if (chan->kernel == CHAN_SCRUB) {
		scrub_fenced(chan);
		/* nobody sleeps on its fence but system sleep's drain and the wake's test fence */
		if (tgm_gate_is_shut(&tgm_sc7_gate))
			nvxnu_wake();
		return 1;
	}
	if (stats_on())
		stats_fences(chan);
	if (chan->kernel) {
		present_fenced(chan);
	} else {
		present_watch_fenced(chan, 0);
	}
	if (READ_ONCE(chan->fenced))
		chan->fenced(chan->fenced_arg);
	return 1;
}

/* The interrupt pass's end of nvkm's handling: every live channel with a kick its pass has not seen done. */
static unsigned chans_completed(void)
{
	unsigned moved = 0;
	u32 i = 0;

	for (i = 0; i < SWITCH_GPU_BUSY_WORDS; i++) {
		uint64_t bits = __atomic_load_n(&tgm_busy.live[i], __ATOMIC_ACQUIRE);

		while (bits) {
			const u32 chid = i * 64u + (u32)__builtin_ctzll(bits);
			struct tgm_chan *chan = NULL;

			bits &= bits - 1u;
			if (!switch_gpu_busy_pending(&tgm_busy, chid))
				continue;
			chan = (struct tgm_chan *)switch_gpu_chan_table_get(&tgm_chan_table, chid);
			if (chan)
				moved += (unsigned)chan_completion(chan);
		}
	}
	return moved;
}
