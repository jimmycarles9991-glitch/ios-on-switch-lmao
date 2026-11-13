// SPDX-License-Identifier: MIT
// What the nonstall pass (SwitchGpuChannelCompletion.h) inlines of the present, the scrubber, the idle clock and the
// statistics on every fence.
#pragma once
#include "gpu_inline.h"

/* push profile (stats):
 * What each push of a submission costs the GPU, sampled: one submission in 64 of each connection (the user client's
 * scanSubmission, with each push's class from the method stream it read, struct tgm_prof) stamps the GPU's time after its
 * pushes, a 16-byte semaphore release after wait-for-idle (push_stamp, as the submission's own start and end stamps) into
 * its own slot of the channel's CHAN_PROF page, each kicked as an entry of its own after the push's. So each push's time
 * is the GPU's from the end of the push before (the start stamp for the first) until its own work has drained, and the
 * fence push (WFI, L2 flush) takes the rest up to the end stamp; a push not stamped (switch_gpu_prof_stamped: a method
 * may continue into the next push, the method stream was not read, or a compositor push that ends no batch) has its time
 * counted with the next one stamped. The stamps' wait for idle keeps pushes from overlapping in a profiled submission,
 * so those run slightly longer than others ("gpuprof cost" gives how much): one in 64 per connection, no more than one in
 * flight per channel, and none with more than TGM_PROF_MAX pushes. A job with internal
 * fences is read once its last seqno completes, against its first seqno's start stamp. Each window, the "gpuprof" line
 * gives every process's time by class (stats_exec_owners), and "gpuprof sequence" its slowest profile push by push. */
static inline void prof_collect(struct tgm_chan *chan, u32 seqno, u64 start, u64 end)
{
	volatile struct tgm_stamp *slot =
		(volatile struct tgm_stamp *)((u8 *)chan->ring + CHAN_PROF);
	u64 prev = start;
	u64 t = 0;
	u32 i = 0;
	u32 n = min_t(u32, READ_ONCE(chan->prof_count), TGM_PROF_MAX);
	u32 draws = 0;
	u32 sdraws = 0;
	u32 ilk = 0;
	u32 syncs = 0;
	u64 sync_ns = 0;
	struct switch_gpu_prof_seq *run = &chan->prof_run;
	struct switch_gpu_prof_push *at = NULL;

	run->first = seqno;
	run->last = READ_ONCE(chan->prof_seqno);
	run->n = run->pushes = 0;
	run->ns = 0;
	for (i = 0; i < n; i++) {
		u8 c = chan->prof_cls[i];
		draws +=
			chan->prof_draws
				[i]; /* a push not stamped: its draws ran in the next stamped one's time */
		sdraws += chan->prof_sdraws[i];
		ilk += chan->prof_ilk[i];
		if (c == TGM_PC_SKIP)
			continue;
		if (c >= TGM_PC_COUNT || slot[i].payload != seqno)
			return; /* not this submission's stamp: drop the rest */
		t = slot[i].ns;
		if (t < prev)
			return;
		if (c == TGM_PC_SYNC) {
			syncs++;
			sync_ns += t - prev;
		}
		__atomic_fetch_add(&chan->prof_ns[c], t - prev, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_n[c], 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_dn[c], draws, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_sdn[c], sdraws, __ATOMIC_RELAXED);
		if (run->n < SWITCH_GPU_PROF_SEQ) {
			at = &run->push[run->n++];
			at->cls = c;
			at->ilk = (u8)min_t(u32, ilk, 0xff);
			at->draws = (u16)min_t(u32, draws, 0xffff);
			at->kpx = chan->prof_kpx[i];
			at->us = (u32)(tgm_deadline_pt_ns(t - prev) / 1000);
		}
		run->pushes++;
		draws = sdraws = ilk = 0;
		prev = t;
	}
	if (end >= prev) {
		__atomic_fetch_add(&chan->prof_ns[TGM_PC_FENCE], end - prev, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_n[TGM_PC_FENCE], 1, __ATOMIC_RELAXED);
		run->ns = tgm_deadline_pt_ns(end - start);
		run->behind = READ_ONCE(chan->stat_kick[seqno % CHAN_NSTAMPS]) != TGM_KICK_CLEAR;
		if (chan->high)
			switch_gpu_compositor_sync(syncs, tgm_deadline_pt_ns(sync_ns), run->behind);
		switch_gpu_prof_seq_offer(&chan->prof_slow, run);
		__atomic_fetch_add(&chan->prof_span, end - start, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_spans, 1, __ATOMIC_RELAXED);
	}
	__atomic_fetch_add(&chan->prof_subs, 1, __ATOMIC_RELAXED);
}

/* A user channel's completed seqno for the job timing: a job's first begins it, each of its seqnos ends a batch, its last
 * ends it and counts its whole time (first start to last end) as the window's exec time. */
static inline void stats_job_seqno(struct tgm_chan *chan, u32 seqno)
{
	volatile struct tgm_stamps *st = chan_stamps(chan, seqno);
	struct switch_gpu_job_run *run = &chan->stat_run;
	u32 i = seqno % CHAN_NSTAMPS;
	u64 span = 0;

	if (READ_ONCE(chan->stat_job[i].first) == seqno) {
		switch_gpu_job_begin(run, seqno, READ_ONCE(chan->stat_job[i].last), st->start.ns);
		run->live = st->start.payload == seqno;
	}
	if (!run->live || (s32)(seqno - run->first) < 0 || (s32)(run->last - seqno) < 0)
		return;
	if (st->end.payload != seqno || st->end.ns < run->prev_pt) {
		run->live = 0;
		return;
	}
	switch_gpu_job_batch(&chan->stat_jobs, run, st->end.ns,
			     tgm_deadline_pt_ns(st->end.ns - run->prev_pt));
	if (!chan->high)
		tgm_hold_released(chan->chid, st->end.ns, seqno == run->last);
	if (seqno != run->last)
		return;
	span = st->end.ns - run->start_pt;
	if (chan->high)
		switch_gpu_compositor_job(tgm_deadline_pt_ns(span));
	stats_add(&tgm_stats.exec_sum, &tgm_stats.exec_max, span);
	__atomic_fetch_add(&chan->stat_exec_n, 1, __ATOMIC_RELAXED);
	stats_add(&chan->stat_exec, &chan->stat_exec_max, span);
	switch_gpu_job_end(&chan->stat_jobs, run);
}

static inline void stats_fences(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);
	u32 s = chan->stat_done;
	u32 i = 0;
	u64 now = nvxnu_nanotime();
	u64 start = 0;
	u64 end = 0;
	volatile struct tgm_stamps *st = NULL;

	if ((s32)(done - s) > CHAN_NSTAMPS) {
		s = done - CHAN_NSTAMPS;
		chan->stat_run.live = 0;
	}
	for (; (s32)(done - s) > 0; s++) {
		i = (s + 1) % CHAN_NSTAMPS;
		if (!chan->kernel) {
			__atomic_fetch_add(&tgm_stats.fences, 1, __ATOMIC_RELAXED);
			stats_add(&tgm_stats.lat_sum, &tgm_stats.lat_max,
				  now - READ_ONCE(chan->stat_sub_ns[i]));
			stats_job_seqno(chan, s + 1);
		}
		st = chan_stamps(chan, s + 1);
		if (!chan->kernel && s + 1 == READ_ONCE(chan->prof_seqno)) {
			u32 first = READ_ONCE(chan->prof_first);
			volatile struct tgm_stamps *fs = chan_stamps(chan, first);

			if (fs->start.payload == first && st->end.payload == s + 1)
				prof_collect(chan, first, fs->start.ns, st->end.ns);
			WRITE_ONCE(chan->prof_seqno, 0);
		}
		if (st->start.payload != s + 1 || st->end.payload != s + 1)
			continue; /* overwritten by a later submission, or not stamped */
		start = st->start.ns;
		end = st->end.ns;
		if (chan->kernel) {
			__atomic_fetch_add(&tgm_stats.copies, 1, __ATOMIC_RELAXED);
			stats_add(&tgm_stats.copy_sum, &tgm_stats.copy_max, end - start);
			continue;
		}
		__atomic_fetch_add(&tgm_stats.timed, 1, __ATOMIC_RELAXED);
		stats_add(&tgm_stats.queue_sum, &tgm_stats.queue_max,
			  start - READ_ONCE(chan->stat_sub_ptimer[i]));
		if (chan->high) {
			__atomic_fetch_add(&tgm_stats.comp, 1, __ATOMIC_RELAXED);
			stats_add(&tgm_stats.comp_sum, &tgm_stats.comp_max,
				  start - READ_ONCE(chan->stat_sub_ptimer[i]));
			if (READ_ONCE(chan->stat_kick[i]) != TGM_KICK_CLEAR) {
				__atomic_fetch_add(&tgm_stats.behind, 1, __ATOMIC_RELAXED);
				stats_add(&tgm_stats.behind_sum, &tgm_stats.behind_max,
					  start - READ_ONCE(chan->stat_sub_ptimer[i]));
			}
		}
	}
	chan->stat_done = done;
	tgm_stats_gr_context();
}

/* A high channel's fence moved from `from` to `done` (interrupt pass): the GPU time of what completed, for the
 * compositor's time per frame (deadline.c): from its start stamp to its end stamp (debug kernels) or now, or for the
 * present (no stamp outside debug kernels) from its kick, then biased by any app on the GPU at its kicks. And each job's
 * start stamp after an app channel it was queued behind: the time that app took to leave the GPU (its preemption
 * latency, hold.c). A job with acquires before its stamp started late for its own reasons. A pass covers whole jobs (only
 * a job's own fence interrupts) unless another channel's interrupt ran it mid-job. */
static inline void high_fenced(struct tgm_chan *chan, u32 from, u32 done)
{
	volatile struct tgm_stamps *st = chan_stamps(chan, from + 1);
	volatile struct tgm_stamps *last = chan_stamps(chan, done);
	struct switch_gpu_kicked_job jobs[SWITCH_GPU_JOBS];
	u64 now = nvxnu_nanotime();
	u64 now_pt = nvkm_timer_read(tgm_gpu.device->timer);
	u64 sample = 0;
	u64 end_pt = 0;
	u32 n = switch_gpu_jobs_span(&chan->jobs, from + 1, done, jobs);
	int biased = !n;
	u32 i = 0;

	tgm_deadline_clock(now, now_pt);
	end_pt = stats_on() && last->end.payload == done ? last->end.ns : now_pt;
	if (!chan->kernel && n && st->start.payload == from + 1)
		sample = tgm_deadline_pt_ns(tgm_gpu_span(end_pt, st->start.ns, tgm_high_end_pt));
	if (!sample) {
		for (i = 0; !biased && i < n; i++)
			biased = jobs[i].app;
		sample = tgm_gpu_span(now, n ? jobs[0].kick_ns : 0, tgm_high_end_ns);
	}
	tgm_high_end_ns = now;
	tgm_high_end_pt = end_pt;
	tgm_deadline_progress(sample, biased);
	if (chan->kernel)
		return;
	for (i = 0; i < n; i++) {
		st = chan_stamps(chan, jobs[i].first);
		if ((s32)(jobs[i].first - from) > 0 && jobs[i].behind >= 0 && !jobs[i].waits &&
		    st->start.payload == jobs[i].first)
			tgm_hold_sampled(jobs[i].behind, jobs[i].asked_pt, st->start.ns);
	}
}

/* From the present channel's nonstall interrupt (the interrupt pass, nvkm's event lock held): a present whose fence passed
 * is done, and its completion work item reads its producers' result and calls back. completing covers the claim to the
 * callback's return, so tgm_present_outstanding never answers 0 for a present whose completion is not delivered yet. */
static inline void present_fenced(struct tgm_chan *chan)
{
	u32 pending = __atomic_load_n(&tgm_present.pending, __ATOMIC_ACQUIRE);
	u64 queued = 0;
	u32 redo = 0;

	if (!pending || (s32)(tgm_chan_completed(chan) - pending) < 0 ||
	    __atomic_load_n(&tgm_present.redoing, __ATOMIC_ACQUIRE))
		return;
	redo = pending;
	if (__atomic_compare_exchange_n(&tgm_present.redo, &redo, 0, false, __ATOMIC_SEQ_CST,
					__ATOMIC_RELAXED)) {
		/* a copy engine reset cut its copy: pending stays set, so the present is outstanding and present_copy
		 * refuses the next, until tgm_present_redo_work has pushed the copy again */
		__atomic_store_n(&tgm_present.redoing, 1, __ATOMIC_RELEASE);
		nvxnu_work_cancel(tgm_present.deadline_work);
		nvxnu_work_schedule(tgm_present.redo_work);
		return;
	}
	queued = tgm_present.queued;
	__atomic_fetch_add(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_compare_exchange_n(&tgm_present.pending, &pending, 0, false, __ATOMIC_SEQ_CST,
					 __ATOMIC_RELAXED)) {
		__atomic_fetch_sub(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
		return;
	}
	nvxnu_work_cancel(tgm_present.deadline_work);
	redo = pending; /* a mark set as its fence passed (tgm_present_redo): the copy was whole */
	__atomic_compare_exchange_n(&tgm_present.redo, &redo, 0, false, __ATOMIC_SEQ_CST,
				    __ATOMIC_RELAXED);
	if (stats_on()) {
		__atomic_fetch_add(&tgm_present.presents, 1, __ATOMIC_RELAXED);
		stats_add(&tgm_present.lat_sum, &tgm_present.lat_max,
			  nvxnu_nanotime() - tgm_present.queued);
	}
	tgm_deadline_presented(queued);
	nvxnu_work_schedule(tgm_present.completion_work);
}

/* A user channel's fence moved, in its interrupt pass or by its kill: a present watching its producer is due
 * (SwitchGpuPresentWatch.h). The pass's end delivers it (tgm_present_pass); a kill runs outside a pass, so it hands the
 * watch to the work item. */
static inline void present_watch_fenced(struct tgm_chan *chan, int killed)
{
	if (switch_gpu_present_watch_fenced(&tgm_present.watch, chan->id,
					    tgm_chan_completed(chan)) &&
	    killed)
		nvxnu_work_schedule(tgm_present.watch_work);
}

/* From the scrub channel's nonstall interrupt: every range whose fence passed goes back clean, or dirty when a copy engine
 * reset may have cut its zeroing (tgm_scrub_redo); then more claims. */
static inline void scrub_fenced(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);
	u32 tail = tgm_scrub.tail;
	u32 head = __atomic_load_n(&tgm_scrub.head, __ATOMIC_ACQUIRE);
	u64 now = stats_on() ? nvxnu_nanotime() : 0;
	u64 took = 0;
	int freed = 0;
	int redo = __atomic_load_n(&tgm_scrub.redo, __ATOMIC_ACQUIRE) != 0;

	while (tail != head && (s32)(done - tgm_scrub.job[tail % SCRUB_JOBS].seqno) >= 0) {
		u32 slot = tail % SCRUB_JOBS;
		if (redo && (s32)(tgm_scrub.redo_through - tgm_scrub.job[slot].seqno) >= 0)
			nvxnu_pool_free_dirty(tgm_scrub.job[slot].pa, tgm_scrub.job[slot].bytes);
		else
			nvxnu_pool_free_zeroed(tgm_scrub.job[slot].pa, tgm_scrub.job[slot].bytes);
		__atomic_fetch_sub(&tgm_scrub.inflight, tgm_scrub.job[slot].bytes,
				   __ATOMIC_RELAXED);
		if (stats_on()) {
			took = now - tgm_scrub.job[slot].queued;
			__atomic_fetch_add(&tgm_scrub.n, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&tgm_scrub.bytes, tgm_scrub.job[slot].bytes,
					   __ATOMIC_RELAXED);
			stats_add(&tgm_scrub.ns, &tgm_scrub.max_ns, took);
		}
		__atomic_store_n(&tgm_scrub.tail, ++tail, __ATOMIC_RELEASE);
		freed = 1;
	}
	if (redo && (s32)(done - tgm_scrub.redo_through) >= 0)
		__atomic_store_n(&tgm_scrub.redo, 0, __ATOMIC_RELEASE);
	if (tail == head)
		nvxnu_work_cancel(tgm_scrub.deadline);
	if (freed && nvxnu_pool_dirty_free_pages())
		nvxnu_work_schedule(tgm_scrub.work);
}
