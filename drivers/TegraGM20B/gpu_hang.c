// SPDX-License-Identifier: MIT
// Channel progress, hang detection, client fault isolation and recovery.
#include "gpu_internal.h"
#include "../../kits/SwitchLinux/SwitchLinux.h"

/* The rule's bookkeeping at a check, every TGM_HANG_NS: the next check's interval, and in *stalled the time the channel
 * has completed nothing when that is TGM_HANG_NS or more (the hang rule's look is due). The time the hold kept the
 * channel off the GPU is not the channel's: it never counts toward a stall. */
static u64 progress_step(struct tgm_chan *chan, u64 *stalled)
{
	u32 done = tgm_chan_completed(chan);
	u64 now = nvxnu_nanotime();
	u64 since = READ_ONCE(chan->stall_ns);
	u64 held = tgm_hold_held_ns(chan->chid, now);
	u64 unheld = 0;

	*stalled = 0;
	if ((s32)(done - chan->submitted) >= 0) {
		WRITE_ONCE(chan->stall_ns, 0);
		return 0;
	}
	if (!since || READ_ONCE(chan->stall_done) != done) {
		WRITE_ONCE(chan->stall_done, done);
		WRITE_ONCE(chan->stall_held, held);
		WRITE_ONCE(chan->stall_ns, now);
		return TGM_HANG_NS;
	}
	unheld = tgm_unheld_ns(now, since, held, READ_ONCE(chan->stall_held));
	if (unheld < TGM_HANG_NS)
		return TGM_HANG_NS - unheld;
	*stalled = unheld;
	WRITE_ONCE(chan->stall_held, held);
	WRITE_ONCE(chan->stall_ns, now);
	return TGM_HANG_NS;
}

uint64_t tgm_chan_check_progress(struct tgm_chan *chan, const char *who, uint32_t seqno)
{
	u64 stalled = 0;
	const u64 next = progress_step(chan, &stalled);

	if (stalled)
		tgm_chan_stalled(chan, who, seqno, stalled);
	return next;
}

uint64_t tgm_chan_check_progress_locked(struct tgm_chan *chan, const char *who, uint32_t seqno)
{
	u64 stalled = 0;
	const u64 next = progress_step(chan, &stalled);

	if (stalled && tgm_hang_recover(chan, who, seqno, stalled) == TGM_HANG_STUCK)
		tgm_chan_hung(chan, who, seqno, stalled);
	return next;
}

int tgm_all_clients(void)
{
	return 1; /* every process that asks gets a device */
}
struct iso_state tgm_iso;

static u32 fault_code(const char *report)
{
	if (!strncmp(report, HANG_REPORT, strlen(HANG_REPORT)) || strstr(report, "CTXSW_TIMEOUT"))
		return 4;
	if (strstr(report, "fault "))
		return 1;
	if (strstr(report, "TRAP") || strstr(report, "ILLEGAL_") || strstr(report, "DATA_ERROR"))
		return TGM_KILL_GR_ERROR;
	return 3;
}

/* nvkm killed the channel (nvkm_chan_error, from its recovery work). Before M3, M3 reads chan->error. From M3 on: an
 * isolated client channel is dead (see above); any other panics, with the report that started it if the interrupt pass
 * did not panic first. */
int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc)
{
	struct tgm_chan *chan = container_of(event, struct tgm_chan, kill);
	const char *report = chan->fault[0] ? chan->fault : nvxnu_gpu_errors();
	char state[TGM_HANG_LINE];
	u32 first = 0;

	(void)repv;
	(void)repc;
	if (tgm_gpu.stage >= 3 && (chan->kernel || !tgm_all_clients())) {
		tgm_chan_describe(chan, state, sizeof(state));
		nvxnu_panic("tegra-gm20b: channel killed by nvkm recovery after \"%s\": %s",
			    nvxnu_gpu_errors(), state);
	}
	if (tgm_gpu.stage < 3) {
		if (!chan->error)
			chan->error = 3;
		return NVIF_EVENT_DROP;
	}
	/* ERRORED is delivered after runlist recovery quiesces the channel. Publish
	 * its failure before advancing abandoned fences or releasing memory leases. */
#if TGM_STATS
	WRITE_ONCE(chan->killed_ns, nvxnu_nanotime());
#endif
	if (fault_code(report) == 1)
		WRITE_ONCE(chan->fault_va, tgm_fault_va(report, (int)chan->chid));
	WRITE_ONCE(chan->error, fault_code(report));
	first = tgm_kill_first(chan->faulted, chan->fault_seqno, READ_ONCE(*chan->fence_cpu));
	switch_gpu_timeline_failed(chan->timeline, first - 1);
	tgm_fence_kill_record(chan->fence_cpu, first, chan->submitted, chan->error);
	__atomic_store_n(chan->fence_cpu, chan->submitted, __ATOMIC_RELEASE);
	__asm__ volatile("dsb sy" ::: "memory");
	present_watch_fenced(chan, 1);
	nvxnu_log("tegra-gm20b: GPU error in %s (channel %u): channel killed, GPU running: %s\n",
		  chan->owner, chan->chid, report);
	nvxnu_wake();
	chan_wake_all_waiters(chan);
	if (READ_ONCE(chan->fenced))
		chan->fenced(chan->fenced_arg);
	return NVIF_EVENT_DROP;
}

/* The next channel id a report names after p ("channel N [" or "ch N"), -2 when there is none; -1 is nvkm's "unknown". */
static const char *next_chid(const char *p, int *chid)
{
	static const char *const tags[] = {"channel ", "ch "};
	const char *best = NULL;
	const char *at = NULL;
	unsigned int i = 0;
	unsigned int len = 0;

	for (i = 0; i < ARRAY_SIZE(tags); i++) {
		for (at = p; (at = strstr(at, tags[i])); at++) {
			if (at == p || at[-1] == ' ' || at[-1] == '|')
				break;
		}
		if (at && (!best || at < best)) {
			best = at;
			len = strlen(tags[i]);
		}
	}
	if (!best)
		return NULL;
	best += len;
	*chid = -2;
	if (*best == '-' || (*best >= '0' && *best <= '9'))
		*chid = (int)simple_strtol(best, (char **)&best, 10);
	return best;
}

/* 1 if nvkm marked the channel's group for recovery (nvkm_runl_rc_cgrp). */
static int chan_marked(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;
	int marked = 0;

	nvkm_runl_foreach(runl, tgm_gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (!nchan)
			continue;
		marked = atomic_read(&nchan->cgrp->rc) != NVKM_CGRP_RC_NONE;
		nvkm_chan_put(&nchan, flags);
		break;
	}
	return marked;
}

static void chan_recover(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;

	nvkm_runl_foreach(runl, tgm_gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			nvkm_runl_rc_cgrp(nchan->cgrp); /* a no-op if nvkm already started it */
			nvkm_chan_put(&nchan, flags);
			return;
		}
	}
}

/* tgm_live_lock held */
void tgm_chan_isolate(struct tgm_chan *chan, const char *report, u64 now)
{
	if (chan->isolating || chan->error)
		return;
	chan->isolating = 1;
	chan->isolated_ns = now;
	tgm_hold_drop(chan->chid);
	strscpy(chan->fault, report, sizeof(chan->fault));
	chan_recover(chan->chid);
	nvxnu_work_schedule_after(tgm_iso.deadline, TGM_ISOLATE_NS);
}

/* From the GPU interrupt work loop after a pass with error reports (nvxnu_errors_fatal's filter): 1 if they stay with
 * client channels, whose kills are then under way; 0 to panic. */
int tgm_errors_contained(const char *errors)
{
	struct tgm_chan *named[8];
	struct tgm_chan *chan = NULL;
	struct tgm_chan *found = NULL;
	const char *p = errors;
	unsigned int n = 0;
	unsigned int i = 0;
	int contained = 1;
	int chid = 0;
	u64 now = nvxnu_nanotime();

	mutex_lock(&tgm_live_lock);
	while (contained) {
		p = next_chid(p, &chid);
		if (!p)
			break;
		if (chid == -2)
			continue; /* "ch" with no number: not an attribution */
		found = NULL;
		list_for_each_entry(chan, &tgm_live_chans, live)
		{
			if ((int)chan->chid == chid) {
				found = chan;
				break;
			}
		}
		if (!found)
			continue; /* no live channel: nvkm's own handling of it is all there is */
		if (found->kernel)
			contained = 0;
		for (i = 0; contained && i < n && named[i] != found; i++)
			;
		if (contained && i == n && n < ARRAY_SIZE(named))
			named[n++] = found;
	}
	/* channels nvkm put into recovery itself, not yet dead or isolated */
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (!contained || chan->error || chan->isolating || !chan_marked(chan->chid))
			continue;
		for (i = 0; i < n && named[i] != chan; i++)
			;
		if (chan->kernel) {
			contained = 0;
		} else if (i == n && n < ARRAY_SIZE(named)) {
			named[n++] = chan;
		}
	}
	if (!n && contained)
		nvxnu_log("tegra-gm20b: GPU error naming no live channel, nothing killed: %s\n",
			  errors);
	for (i = 0; contained && i < n; i++)
		tgm_chan_isolate(named[i], errors, now);
	mutex_unlock(&tgm_live_lock);
	return contained;
}

void tgm_iso_deadline(void *arg)
{
	struct tgm_chan *chan = NULL;
	char state[TGM_HANG_LINE];
	u64 now = nvxnu_nanotime();
	int again = 0;

	(void)arg;
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (!chan->isolating || READ_ONCE(chan->error) ||
		    now - chan->isolated_ns < TGM_ISOLATE_NS)
			continue;
		switch (tgm_iso_verdict(chan_marked(chan->chid), now - chan->isolated_ns,
					chan->iso_retried)) {
		case TGM_ISO_WAIT:
			again = 1;
			break;
		case TGM_ISO_RETRY:
			chan->iso_retried = 1;
			chan_recover(chan->chid);
			again = 1;
			break;
		case TGM_ISO_LOST:
			tgm_chan_describe(chan, state, sizeof(state));
			nvxnu_panic("tegra-gm20b: GPU error in %s not recovered %llu ms after it "
				    "(\"%s\"): %s",
				    chan->owner, (now - chan->isolated_ns) / NSEC_PER_MSEC,
				    chan->fault, state);
		}
	}
	if (again)
		nvxnu_work_schedule_after(tgm_iso.deadline, TGM_ISOLATE_NS);
	mutex_unlock(&tgm_live_lock);
}

/* tgm_live_lock held. Internal fences move the fence word without an interrupt: a move the interrupt pass has not seen
 * counts as progress now, when a check first sees it. */
static u64 chan_progress_ns(struct tgm_chan *chan, u64 now)
{
	const u32 done = tgm_chan_completed(chan);

	if (tgm_progress_seen(done, READ_ONCE(chan->progress_done), &chan->seen_done)) {
		WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, now));
		WRITE_ONCE(chan->progress_ns, now);
	}
	return max(READ_ONCE(chan->progress_ns), READ_ONCE(chan->busy_ns));
}

/* The hold's time since the channel's last progress is left out, as tgm_chan_check_progress leaves it out. */
static int chan_stuck_for(struct tgm_chan *chan, u64 now, u64 window)
{
	u64 since = chan_progress_ns(chan, now);
	u64 unheld = tgm_unheld_ns(now, since, tgm_hold_held_ns(chan->chid, now),
				   READ_ONCE(chan->progress_held));

	return tgm_stuck_for(READ_ONCE(chan->error) != 0, tgm_chan_completed(chan),
			     READ_ONCE(chan->submitted), since + unheld, since, window);
}

static int chan_stuck(struct tgm_chan *chan, u64 now)
{
	return chan_stuck_for(chan, now, TGM_STUCK_NS);
}

static int chan_unowned(struct tgm_chan *chan)
{
	return tgm_switch_unowned(READ_ONCE(chan->error) != 0, tgm_chan_completed(chan),
				  READ_ONCE(chan->submitted));
}

/* Whether the channel's fence moved within the stuck time, or its GET did since tgm_hang_recover last read it (a long job
 * whose methods the engine still takes moves GET, not the fence). USERD's GP_GET and GET are what the host wrote back. */
static int chan_moving(struct tgm_chan *chan, u64 now)
{
	volatile u32 *userd = chan->user.map.ptr;
	const u64 get =
		userd ? (u64)userd[0x88 / 4] << 40 ^ (u64)userd[0x44 / 4] << 32 ^ userd[0x40 / 4]
		      : 0;

	if (get != chan->get_seen) {
		chan->get_seen = get;
		chan->get_ns = now;
	}
	return now - max(chan_progress_ns(chan, now), chan->get_ns) < TGM_STUCK_NS;
}

/* A wait on `late` (the present's or the scrubber's channel when its own work is late) ran out with no GPU error
 * reported. The channel to blame (TegraGM20BRecover.h tgm_hang_blame) is the first stuck one of what the engines and
 * PBDMAs are on (a context switch that does not finish blames the context nvkm's own recovery would), the channel of its
 * task `late` still waits on (tgm_chan_order), and `late`. A client's is killed as a faulting client's is, a kernel
 * channel's engines are reset (tgm_kernel_hang_locked), and the waits it held up go on. tgm_live_lock held. */
#if TGM_STATS
/* tgm_live_lock held: the blamed channel's own order wait and the late one's progress, as a line of its
 * own (TegraGM20BHangDecode.h tgm_hang_detail) */
static void hang_detail(struct tgm_chan *blame, struct tgm_chan *late, u64 now)
{
	const struct tgm_order *w = &blame->waited;
	struct tgm_hang_detail d = {0};
	struct tgm_chan *source = NULL;
	const volatile u32 *bind = NULL;
	char line[TGM_HANG_LINE];

	d.chid = blame->chid;
	d.stalled_ms = (now - chan_progress_ns(blame, now)) / 1000000;
	if (w->cpu && (s32)(READ_ONCE(*w->cpu) - w->seqno) < 0) {
		d.waits = 1;
		d.wait_chid = w->chid;
		d.wait_seqno = w->seqno;
		d.wait_at = READ_ONCE(*w->cpu);
		source = live_by(w->id, 0);
	}
	if (source) {
		d.source_live = 1;
		d.source_done = tgm_chan_completed(source);
		d.source_submitted = READ_ONCE(source->submitted);
		d.source_ms = (now - chan_progress_ns(source, now)) / 1000000;
	}
	d.late_chid = late->chid;
	d.late_done = tgm_chan_completed(late);
	d.late_submitted = READ_ONCE(late->submitted);
	d.late_ms = (now - chan_progress_ns(late, now)) / 1000000;
	tgm_hang_detail_format(line, sizeof(line), &d);
	bind = blame->vm ? __atomic_load_n(&blame->vm->bind_cpu, __ATOMIC_ACQUIRE) : NULL;
	nvxnu_log("tegra-gm20b: GPU hang detail: %s; bind epoch %u of %u\n", line,
		  bind ? READ_ONCE(*bind) : 0, bind ? READ_ONCE(blame->vm->bind_target) : 0);
}

#endif
/* What the candidates wait on (tgm_chan_wait_source), read once a channel and check */
struct candidate_waits {
	struct tgm_chan *chan[8], *source[8];
	enum tgm_wait_source kind[8];
	int n;
};

/* tgm_live_lock held */
static enum tgm_wait_source candidate_wait(struct candidate_waits *w, struct tgm_chan *chan,
					   struct tgm_chan **source)
{
	int i = 0;

	for (i = 0; i < w->n && w->chan[i] != chan; i++)
		;
	if (i == w->n && w->n < (int)ARRAY_SIZE(w->chan)) {
		w->chan[i] = chan;
		w->kind[i] = tgm_chan_wait_source(chan, &w->source[i], NULL, 0);
		w->n++;
	}
	*source = i < w->n ? w->source[i] : NULL;
	return i < w->n ? w->kind[i] : TGM_WAITS_NONE;
}

/* tgm_live_lock: the line names the channel and the engine, past nvxnu_log's 512 characters and within os_log's 1,024 */
static char ctxsw_line[1024];

enum tgm_hang tgm_hang_recover(struct tgm_chan *late, const char *who, u32 seqno, u64 waited_ns)
{
	struct tgm_hang_candidate c[8] = {};
	struct tgm_chan *chans[8] = {};
	struct candidate_waits waits = {};
	struct tgm_chan *source = NULL;
	enum tgm_wait_source kind = TGM_WAITS_NONE;
	struct tgm_chan *chan = NULL;
	struct tgm_chan *blame = NULL;
	char report[sizeof(late->fault)];
	char state[TGM_HANG_LINE];
	enum tgm_hang verdict = TGM_HANG_STUCK;
	u64 now = nvxnu_nanotime();
	u64 moved_ns = 0;
	int suspects[4];
	int resident = 0;
	int n = 0;
	int i = 0;
	int j = 0;
	int b = 0;
	int progress = 0;

	if (tgm_gpu.stage < 3 || !tgm_iso.deadline)
		return TGM_HANG_STUCK;
	resident = tgm_recover_suspects(tgm_gpu.device, suspects, ARRAY_SIZE(suspects));
	for (n = 0; n < resident; n++)
		chans[n] = live_by(0, suspects[n]);
	/* the late one's acquires, source by source, up to two hops, then the late one itself last */
	for (chan = late, i = 0; chan && i < 2; i++) {
		if (candidate_wait(&waits, chan, &source) != TGM_WAITS_CHANNEL || !source)
			break;
		chans[n++] = source;
		chan = source;
	}
	chans[n] = late;
	for (i = 0; i <= n; i++) {
		c[i].live = chans[i] != NULL;
		c[i].dead = !chans[i] || READ_ONCE(chans[i]->error) != 0;
		c[i].kernel = chans[i] && chans[i]->kernel;
		c[i].resident = i < resident;
		c[i].loading = chans[i] && tgm_recover_loading(tgm_gpu.device, (int)chans[i]->chid);
		c[i].saving = chans[i] && tgm_recover_saving(tgm_gpu.device, (int)chans[i]->chid);
		c[i].moving = chans[i] && chan_moving(chans[i], now);
		for (j = 0; j < i && chans[j] != chans[i]; j++)
			;
		/* once per channel and check: a channel among both suspects and sources is one observation */
		if (j < i)
			c[i].stuck = c[j].stuck;
		else
			c[i].stuck =
				chans[i] &&
				tgm_hang_confirm((chan_stuck(chans[i], now) ||
						  (c[i].saving && chan_unowned(chans[i]))) &&
							 !c[i].moving,
						 tgm_chan_completed(chans[i]),
						 &chans[i]->hang_strikes, &chans[i]->hang_seen);
	}
	for (i = 0; i <= n; i++) {
		if (!chans[i])
			continue;
		kind = candidate_wait(&waits, chans[i], &source);
		c[i].victim = kind == TGM_WAITS_VICTIM || kind == TGM_WAITS_UNREAD;
		tgm_acquire_unfired(chans[i], now);
#if TGM_STATS
		tgm_acquire_probe_look(chans[i], now);
#endif
		for (j = 0; source && j <= n; j++)
			if (j != i && chans[j] == source)
				c[i].waits = (u8)(j + 1);
	}
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (now - chan_progress_ns(chan, now) < TGM_STUCK_NS)
			progress = 1;
		if (!READ_ONCE(chan->error))
			moved_ns = max(moved_ns, READ_ONCE(chan->progress_ns));
	}
	/* each candidate's engine reset is tried at most once a check */
	for (;;) {
		verdict = tgm_hang_blame(c, n + 1, READ_ONCE(late->error) != 0, late->kernel,
					 progress, &b);
		if (verdict == TGM_HANG_RESIDUE) {
			if (tgm_recover_residue(tgm_gpu.device, suspects[b])) {
				nvxnu_log(
					"tegra-gm20b: GPU hang: %s waited %llu ms for %s fence %u "
					"behind dead channel %d: its engine reset, nothing "
					"killed\n",
					who, waited_ns / 1000000, late->owner, seqno, suspects[b]);
				return TGM_HANG_RECOVERING;
			}
			c[b].dead = 0;
		} else if (verdict == TGM_HANG_RELOAD) {
			if (tgm_load_recover_locked(chans[b], who))
				return TGM_HANG_RECOVERING;
			c[b].loading = 0;
		} else {
			break;
		}
	}
	if (verdict == TGM_HANG_KERNEL)
		return tgm_kernel_hang_locked(chans[b], late, who, seqno, waited_ns, moved_ns);
	if (b < 0 || chans[b]->isolating)
		return verdict;
	blame = chans[b];
	tgm_chan_describe(blame, state, sizeof(state));
	scnprintf(report, sizeof(report), HANG_REPORT "%s waited %llu ms for %s fence %u; %s", who,
		  waited_ns / 1000000, late->owner, seqno, state);
	nvxnu_log(
		"tegra-gm20b: GPU hang, recovering %s (channel %u): %s waited %llu ms for %s fence "
		"%u; %s\n",
		blame->owner, blame->chid, who, waited_ns / 1000000, late->owner, seqno, state);
#if TGM_STATS
	hang_detail(blame, late, now);
#endif
	tgm_chan_isolate(blame, report, now);
	tgm_recover_fault(tgm_gpu.device, blame->chid);
	return verdict;
}

/* Progress after the reset is the redone work completing when there is some, else any job completing. */
void tgm_ctxsw_redo_mark(struct tgm_chan *chan)
{
	const int marked =
		chan->kernel == CHAN_SCRUB ? tgm_scrub_redo(chan->submitted) : tgm_present_redo();

	chan->ctxsw_reset = 1;
	chan->ctxsw_reset_after = marked ? chan->submitted : tgm_chan_completed(chan);
}

/* A client's context that cannot switch is the only one lost: nvkm's recovery (nvkm_runl_rc) takes that channel off
 * the runlist and resets the engines still on its group; every other channel's context lives in its own instance
 * block, GR's reset reloads its firmware with clock gating off (tgm_clkgate_guard_reset) and ZBC from nvkm's tables, and
 * the runlist goes on without the dead channel. */
enum tgm_ctxsw tgm_ctxsw_decide(int chid, const char *engine_name, int gr)
{
	struct tgm_chan *chan = NULL;
	char report[sizeof(chan->fault)];
	char state[TGM_HANG_LINE];
	char engine[TGM_HANG_LINE];
	char engines[TGM_HANG_LINE];
	u64 now = nvxnu_nanotime();
	enum tgm_ctxsw verdict = TGM_CTXSW_RESET;
	u32 killed = 0;

	mutex_lock(&tgm_live_lock);
	chan = live_by(0, chid);
	if (chan) {
		verdict = tgm_ctxsw_verdict(
			1, READ_ONCE(chan->error) != 0, chan->kernel,
			chan_stuck_for(chan, now, tgm_stuck_window(gr)) || chan_unowned(chan),
			tgm_ctxsw_redo_pending(chan->ctxsw_reset, chan->ctxsw_reset_after,
					       tgm_chan_completed(chan)));
		if (chan->kernel && (verdict == TGM_CTXSW_REDO || verdict == TGM_CTXSW_FATAL))
			verdict = tgm_kernel_stall_locked(
				chan, engine_name, verdict == TGM_CTXSW_FATAL, 0, 1, &killed);
		if (verdict == TGM_CTXSW_KILL && !chan->isolating) {
			tgm_chan_describe(chan, state, sizeof(state));
			scnprintf(report, sizeof(report), "CTXSW_TIMEOUT: %s", state);
			tgm_chan_isolate(chan, report, now);
			tgm_engine_state(engine, sizeof(engine));
			tgm_engines_line(engines, sizeof(engines));
			scnprintf(ctxsw_line, sizeof(ctxsw_line),
				  "tegra-gm20b: context switch timed out, recovering %s (channel "
				  "%u): %s; engines %s; %s\n",
				  chan->owner, chan->chid, state, engines, engine);
			switch_linux_log_line(ctxsw_line);
		}
	}
	mutex_unlock(&tgm_live_lock);
	return verdict;
}

void tgm_chan_stalled(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
	enum tgm_hang verdict = TGM_HANG_STUCK;

	mutex_lock(&tgm_live_lock);
	verdict = tgm_hang_recover(chan, who, seqno, waited_ns);
	mutex_unlock(&tgm_live_lock);
	if (verdict == TGM_HANG_STUCK)
		tgm_chan_hung(chan, who, seqno, waited_ns);
}
