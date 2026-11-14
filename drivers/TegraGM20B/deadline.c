// SPDX-License-Identifier: MIT
/*
 * The compositor's deadline (TegraGM20BSched.h). The display sends Core Animation a vsync only while CA has vsync on (it
 * turns it on when it schedules an update, off after an idle countdown: QuartzCore IOMFBServer::set_vsync_enabled and
 * vsync_callback), and CA starts a frame at it for the vblank after the one it came with; the display passes each one on
 * here with its real vblank (tegra_gm20b_compositor_vsync). 24B119's CA never turns it on (IOMFBServer's use-VBL bit is
 * cleared at construction and nothing sets it; its frame clock is a CADisplayTimer), so there the display passes each
 * frame end on instead. A window then holds each app channel slow enough to be worth it (tgm_hold_worth) from its own
 * preemption latency before the compositor's latest start (that vblank less the compositor's GPU time per frame) until the
 * frame's present completes (hold.c); with no such channel no window opens. Nor does one while none of them has work in
 * flight (tgm_busy): the vblank it would aim for is kept, and the first such channel's kick before it opens the window
 * and takes the holds due by then before its doorbell, as an open window's hold would have found it. The GPU time per
 * frame is learned from the high channels' completions (gpu_fenced.h
 * high_fenced), summed per present. QuartzCore's own hint (tegra_gm20b_display_deadline) comes once a second
 * (hint_agx_timing::next_agx_hint), too seldom to start a window from. Every decision is made at a vsync, a kick, a
 * fence interrupt or a host-time callout; nothing waits or polls.
 *
 * The window and the holds belong to the GPU interrupt work loop's gate (nvxnu_pass_run): the interrupt pass closes a
 * window at its present there, and the owner's runs open, time and expire windows there. A run comes from a wake (the
 * frame end, or a kick or a hold change that found the gate held) or from the owner's timer, which only the gate's
 * holder arms, at the earliest of the next hold, a preempt's check and the window's expiry. The frame end and the kicks
 * publish their own words with plain stores, each word one writer's.
 */
#include <core/device.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BSched.h"
#include "TegraGM20BSchedPolicy.h"
#include "deadline_stats.h"
#include "SwitchGpuKeptWindow.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

static struct {
	struct nvkm_device *device;
	void *wake_work, *timer_work;
	/* the frame end's (the display's work loop): the frame interval, the vblank the frame CA started at the last vsync
	 * aims for (published last), the last vsync to CA and the start of its unbroken run */
	u64 interval, next_target, vsync_last, vsync_since;
	struct switch_gpu_kept_window kept;
	/* the high kicks' own, plain stores and no lock (a kick's time is a hint a racing kick may set a few us back): the
	 * compositor's last kick, the last kick that found an app on the GPU, the vblank the frame's first kick aimed for */
	u64 kick_last, kick_app_last, frame_target;
	/* the interrupt pass's own: the compositor's GPU time since the present that moved frame_epoch to pass_epoch, and
	 * whether an app biased it; the PTIMER anchor and rate, anchored again when resume moves recal */
	u64 pass_ns, cal_ns, cal_pt, pt_rate;
	u32 pass_epoch, recal_seen;
	int pass_biased;
	/* system sleep's and the wake's */
	u32 recal;
	int suspended;

	/* the gate's. A present's: when the last one completed, the frame epoch it closed, and whether a work item completed
	 * it (a kill's, a late look's): a pass may still add that frame's last job to the next frame's time */
	u64 presented_at, expected;
	u32 frame_epoch;
	int tail;
	/* the window: the vblank it aims for (0: none), when it opened (the compositor kicked in it when kick_last is
	 * later), its expiry, its next hold (0: none left); a preempt's check; when the timer is armed for (0: not) */
	u64 target, opened, expire_at, hold_at, check_at, armed_at;
	int check_chid;
	/* counts that only grow (deadline_stats.h) */
	u32 frames, missed_app, missed_clear, windows, empty, expired, kick_opens, frame_end_runs,
		timer_runs, wake_runs;
} dl;

static void wake_work(void *arg);
static void timer_work(void *arg);

void tgm_deadline_init(struct nvkm_device *device)
{
	if (dl.device)
		return;
	dl.wake_work = nvxnu_work_alloc(wake_work, NULL);
	dl.timer_work = nvxnu_work_alloc(timer_work, NULL);
	if (!dl.wake_work || !dl.timer_work)
		nvxnu_panic("tegra-gm20b: deadline: no thread calls");
	WRITE_ONCE(dl.device, device);
}

void tgm_deadline_wake(void)
{
	if (READ_ONCE(dl.device))
		nvxnu_work_schedule(dl.wake_work);
}

static u64 latest_target(void)
{
	return __atomic_load_n(&dl.next_target, __ATOMIC_ACQUIRE);
}

static int drawing(u64 now)
{
	return tgm_compositor_drawing(now, READ_ONCE(dl.interval), READ_ONCE(dl.vsync_last),
				      READ_ONCE(dl.vsync_since), READ_ONCE(dl.kick_last));
}

/* The gate held: the timer at the owner's next time, or off with nothing pending. */
static void arm(u64 now)
{
	u64 at = dl.check_at;

	if (dl.target && (!at || dl.expire_at < at))
		at = dl.expire_at;
	if (dl.target && dl.hold_at && dl.hold_at < at)
		at = dl.hold_at;
	if (at == dl.armed_at)
		return;
	dl.armed_at = at;
	if (!at) {
		nvxnu_work_cancel(dl.timer_work);
	} else {
		nvxnu_work_schedule_after(dl.timer_work, at > now ? at - now : 0);
	}
}

/* The gate held: a window for the frame aiming at `target`, opened at `since`; its holds are looked at from now on */
static void window_open(u64 target, u64 since, u64 now)
{
	__atomic_store_n(&dl.target, target, __ATOMIC_RELAXED);
	dl.opened = since;
	dl.expire_at = target;
	dl.hold_at = now ? now : 1;
	switch_gpu_kept_done(&dl.kept, target);
	if (TGM_STATS)
		WRITE_ONCE(dl.windows, dl.windows + 1);
}

/* The gate held */
static void window_close(u64 now)
{
	tgm_hold_release(dl.device, now);
	__atomic_store_n(&dl.target, 0, __ATOMIC_RELAXED);
	dl.hold_at = 0;
}

/* The gate held, no window open: the window for the frame aiming at `target` opens now (from `since`) if a slow app
 * channel has work in flight; with only slow ones that have none it is kept for the first one's kick. Returns the
 * window's vblank. */
static u64 window_due(u64 target, u64 since, u64 now)
{
	const u64 interval = READ_ONCE(dl.interval);
	struct tgm_window w;

	__atomic_store_n(&dl.kept.present, 0, __ATOMIC_RELAXED);
	if (READ_ONCE(dl.suspended) || !interval || interval > TGM_INTERVAL_MAX_NS ||
	    !drawing(now) || !tgm_hold_wanted(0))
		return 0;
	if (!tgm_hold_wanted(1)) {
		/* kept, then looked at again: a kick between the two either sees it or is seen */
		switch_gpu_kept_set(&dl.kept.present, &dl.kept.present_since, target, since);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!tgm_hold_wanted(1))
			return 0;
	}
	window_open(target, since, now);
	w.now = now;
	w.vblank = target;
	w.interval = interval;
	w.expected = dl.expected;
	dl.hold_at = tgm_hold_plan(&w);
	return target;
}

/* The gate held, no window open: the window for the latest frame end, or the one kept for a kick */
static void window_from_frame_end(u64 now)
{
	const u64 next = latest_target();
	u64 since = 0;
	u64 target = switch_gpu_kept_window(&dl.kept, now, next, &since);

	if (!target) {
		target = next;
		since = READ_ONCE(dl.vsync_last);
	}
	if (target > now && target > dl.kept.done)
		window_due(target, since, now);
}

/* In the gate, for the frame end: the window opens and the timer goes to its first hold. Its holds are taken in the
 * owner's run, not on the display's work loop. */
static void frame_end_open(void *arg)
{
	const u64 now = *(const u64 *)arg;

	if (TGM_STATS)
		WRITE_ONCE(dl.frame_end_runs, dl.frame_end_runs + 1);
	if (dl.target || READ_ONCE(dl.suspended))
		return;
	window_from_frame_end(now);
	arm(now);
}

/* The display's work loop. A window wanted for the frame CA started: it opens in the gate here if no pass holds it,
 * else in the owner's run (or a present opens it, when one is still open); kept for a slow app's kick when none of the
 * slow channels has work in flight. */
void tegra_gm20b_compositor_vsync(uint64_t stamp, uint32_t lead_us, uint64_t interval)
{
	struct nvkm_device *device = READ_ONCE(dl.device);
	u64 now = nvxnu_nanotime();
	u64 step = 0;
	u64 target = 0;

	if (!device)
		return;
	step = nvxnu_abstime_to_ns(interval);
	target = nvxnu_abstime_to_ns(stamp) + lead_us * 1000ULL + step;
	if (!dl.vsync_last || now - dl.vsync_last > step * 3 / 2)
		WRITE_ONCE(dl.vsync_since, now);
	WRITE_ONCE(dl.vsync_last, now);
	WRITE_ONCE(dl.interval, step);
	__atomic_store_n(&dl.next_target, target, __ATOMIC_RELEASE);
	if (__atomic_load_n(&dl.target, __ATOMIC_RELAXED))
		return;
	if (READ_ONCE(dl.suspended) || !step || step > TGM_INTERVAL_MAX_NS || !drawing(now) ||
	    !tgm_hold_wanted(0)) {
		__atomic_store_n(&dl.kept.frame_end, 0, __ATOMIC_RELAXED);
		return;
	}
	if (!tgm_hold_wanted(1)) {
		switch_gpu_kept_set(&dl.kept.frame_end, &dl.kept.frame_end_since, target, now);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!tgm_hold_wanted(1))
			return;
	} else {
		__atomic_store_n(&dl.kept.frame_end, 0, __ATOMIC_RELAXED);
	}
	if (!nvxnu_pass_try(frame_end_open, &now))
		nvxnu_work_schedule(dl.wake_work);
}

/* The gate held: the holds due now, the next one's time, and a check half its latency after a preempt of the channel
 * the GPU is on. The resident look reads PFIFO only once a hold's time has come. */
static void holds_due(u64 now)
{
	struct tgm_window w;
	int preempt = -1;

	if (!drawing(now)) {
		dl.hold_at = 0;
		return;
	}
	w.now = now;
	w.vblank = dl.target;
	w.interval = READ_ONCE(dl.interval);
	w.expected = dl.expected;
	dl.hold_at = tgm_hold_due(dl.device, &w, TGM_RESIDENT_LOOK, &preempt);
	if (preempt >= 0 && tgm_hold_preempt(dl.device, (unsigned int)preempt)) {
		dl.check_chid = preempt;
		dl.check_at = now + tgm_hold_latency((unsigned int)preempt) / 2;
	}
}

/* The gate held, at the window's vblank: a window the compositor never kicked in had no frame (CA drew nothing at that
 * vsync) and ends; one with a frame still in flight gets one more interval, then ends. */
static void window_expire(u64 now)
{
	const int kicked = READ_ONCE(dl.kick_last) >= dl.opened;

	if (kicked && dl.expire_at == dl.target) {
		dl.expire_at = dl.target + READ_ONCE(dl.interval);
		return;
	}
	if (TGM_STATS && kicked) {
		WRITE_ONCE(dl.expired, dl.expired + 1);
	} else if (TGM_STATS) {
		WRITE_ONCE(dl.empty, dl.empty + 1);
	}
	window_close(now);
}

/* The owner's run, in the gate. What is due comes from the state, never from which wake ran it. System sleep cancels
 * and waits for the runs after it sets suspended, and they look before they read a register. */
static void period_run(int fired)
{
	const u64 now = nvxnu_nanotime();

	if (fired)
		dl.armed_at = 0;
	if (READ_ONCE(dl.suspended))
		return;
	if (TGM_STATS && fired) {
		WRITE_ONCE(dl.timer_runs, dl.timer_runs + 1);
	} else if (TGM_STATS) {
		WRITE_ONCE(dl.wake_runs, dl.wake_runs + 1);
	}
	tgm_hold_settle(dl.device, now);
	if (dl.target && now >= dl.expire_at)
		window_expire(now);
	if (!dl.target)
		window_from_frame_end(now);
	if (dl.target && dl.hold_at && now >= dl.hold_at)
		holds_due(now);
	if (dl.check_at && now >= dl.check_at) {
		dl.check_at = 0;
		tgm_hold_check(dl.device, (unsigned int)dl.check_chid);
	}
	arm(now);
}

static void period_woken(void *arg)
{
	(void)arg;
	period_run(0);
}

static void period_timed(void *arg)
{
	(void)arg;
	period_run(1);
}

static void wake_work(void *arg)
{
	(void)arg;
	nvxnu_pass_run(period_woken, NULL);
}

static void timer_work(void *arg)
{
	(void)arg;
	nvxnu_pass_run(period_timed, NULL);
}

void tgm_deadline_kicked(int compositor, int app)
{
	const u64 now = nvxnu_nanotime();

	if (!READ_ONCE(dl.device))
		return;
	if (compositor) {
		if (READ_ONCE(dl.kick_last) <= READ_ONCE(dl.presented_at))
			WRITE_ONCE(dl.frame_target, READ_ONCE(dl.next_target));
		WRITE_ONCE(dl.kick_last, now);
	}
	if (app)
		WRITE_ONCE(dl.kick_app_last, now);
}

struct kept_open {
	u64 now;
	int opened;
};

/* In the gate, for the kicking thread: the window the frame end kept, from then (a compositor kick or present since
 * counts), with the holds due by now. */
static void kept_open(void *arg)
{
	struct kept_open *k = arg;
	struct tgm_window w;
	u64 since = 0;
	u64 target = 0;
	int preempt = -1;

	if (dl.target || READ_ONCE(dl.suspended) || !drawing(k->now))
		return;
	target = switch_gpu_kept_window(&dl.kept, k->now, latest_target(), &since);
	if (!target)
		return;
	window_open(target, since, k->now);
	w.now = k->now;
	w.vblank = target;
	w.interval = READ_ONCE(dl.interval);
	w.expected = dl.expected;
	dl.hold_at = tgm_hold_due(dl.device, &w, TGM_VICTIM_NONE, &preempt);
	if (TGM_STATS)
		WRITE_ONCE(dl.kick_opens, dl.kick_opens + 1);
	arm(k->now);
	k->opened = 1;
}

/* The kicking thread, before its doorbell. No slow channel had work at the frame end, so none is on the GPU now and
 * the kicking one is not yet: the resident look reads nothing. A pass or the owner in the gate: the owner opens it
 * next, after the doorbell. */
int tgm_deadline_app_kicked(void)
{
	struct kept_open k = {0, 0};
	u64 since = 0;

	if (!switch_gpu_kept_any(&dl.kept))
		return 0;
	k.now = nvxnu_nanotime();
	if (!switch_gpu_kept_window(&dl.kept, k.now, latest_target(), &since) ||
	    !tgm_hold_wanted(1))
		return 0;
	if (!nvxnu_pass_try(kept_open, &k)) {
		nvxnu_work_schedule(dl.wake_work);
		return 0;
	}
	return k.opened;
}

/* Interrupt pass. The anchor moves on every 2^40 ns (18 minutes), keeping the rate, so the span never overflows. */
void tgm_deadline_clock(uint64_t ns, uint64_t pt)
{
	const u32 recal = READ_ONCE(dl.recal);
	u64 rate = 0;

	if (recal != dl.recal_seen || !dl.cal_ns || pt < dl.cal_pt ||
	    ns - dl.cal_ns >= 1ULL << 40) {
		dl.recal_seen = recal;
		dl.cal_ns = ns;
		dl.cal_pt = pt;
		return;
	}
	rate = tgm_pt_rate(ns - dl.cal_ns, pt - dl.cal_pt);
	if (rate)
		WRITE_ONCE(dl.pt_rate, rate);
}

uint64_t tgm_deadline_pt_ns(uint64_t pt_span)
{
	return tgm_pt_ns(pt_span, READ_ONCE(dl.pt_rate));
}

void tgm_deadline_progress(uint64_t ns, int biased)
{
	const u32 epoch = READ_ONCE(dl.frame_epoch);

	if (dl.pass_epoch != epoch) {
		WRITE_ONCE(dl.pass_ns, 0);
		WRITE_ONCE(dl.pass_biased, 0);
		WRITE_ONCE(dl.pass_epoch, epoch);
	}
	WRITE_ONCE(dl.pass_ns, dl.pass_ns + ns);
	if (biased)
		WRITE_ONCE(dl.pass_biased, 1);
}

struct presented {
	u64 queued;
	int pass;
};

/* The gate held: the window the present queued at `queued` ends, or the window kept for its frame; its vblank, 0 if
 * neither is this frame's. */
static u64 present_ends(u64 queued, u64 now)
{
	u64 since = 0;
	u64 kept = 0;

	if (dl.target) {
		if (queued < dl.opened)
			return 0;
		kept = dl.target;
		window_close(now);
		return kept;
	}
	kept = switch_gpu_kept_window(&dl.kept, now, latest_target(), &since);
	if (!kept || queued < since)
		return 0;
	switch_gpu_kept_done(&dl.kept, kept);
	return kept;
}

/* The gate held: the frame's present completed. */
static void presented_run(void *arg)
{
	const struct presented *p = arg;
	u64 now = nvxnu_nanotime();
	u64 closed = 0;
	/* a compositor kick since the last present: a frame; the pass's time is this frame's while its epoch is ours, whole
	 * only at the pass's end, and only its own after a present the pass completed */
	const int tail = dl.tail;

	dl.tail = !p->pass;
	if (READ_ONCE(dl.kick_last) > dl.presented_at) {
		const u64 frame_target = READ_ONCE(dl.frame_target);
		const u64 frame_ns = READ_ONCE(dl.pass_ns);
		const int ours = READ_ONCE(dl.pass_epoch) == dl.frame_epoch;

		if (TGM_STATS) {
			WRITE_ONCE(dl.frames, dl.frames + 1);
			if (frame_target && now > frame_target &&
			    READ_ONCE(dl.kick_app_last) > dl.presented_at) {
				WRITE_ONCE(dl.missed_app, dl.missed_app + 1);
			} else if (frame_target && now > frame_target) {
				WRITE_ONCE(dl.missed_clear, dl.missed_clear + 1);
			}
		}
		if (p->pass && !tail && ours && !READ_ONCE(dl.pass_biased) && frame_ns) {
			WRITE_ONCE(dl.expected, tgm_expect(dl.expected, frame_ns));
			if (TGM_STATS)
				tgm_deadline_learned(frame_ns);
		}
	}
	WRITE_ONCE(dl.frame_epoch, dl.frame_epoch + 1);
	WRITE_ONCE(dl.presented_at, now);
	tgm_hold_unneed(p->queued);
	closed = present_ends(p->queued, now);
	if (closed) {
		const u64 next = latest_target();

		if (next > closed && next > now)
			window_due(next, now, now);
	}
	arm(now);
}

/* The present completed: the frame's GPU time is learned (unless an app held the GPU at one of its kicks), and the window
 * whose frame this is ends (a present queued before it opened is an earlier frame's). If CA has started the next frame
 * since, its window opens now. The pass is in the gate already. */
void tgm_deadline_presented(uint64_t queued, int pass)
{
	struct presented p = {queued, pass};

	if (!READ_ONCE(dl.device))
		return;
	if (pass) {
		presented_run(&p);
	} else {
		nvxnu_pass_run(presented_run, &p);
	}
}

static void suspend_run(void *arg)
{
	const u64 now = nvxnu_nanotime();

	(void)arg;
	switch_gpu_kept_done(&dl.kept, __atomic_load_n(&dl.kept.frame_end, __ATOMIC_ACQUIRE));
	__atomic_store_n(&dl.kept.present, 0, __ATOMIC_RELAXED);
	if (dl.target)
		window_close(now);
	dl.check_at = 0;
	arm(now);
}

void tgm_deadline_suspend(void)
{
	if (!READ_ONCE(dl.device))
		return;
	WRITE_ONCE(dl.suspended, 1);
	nvxnu_pass_run(suspend_run, NULL);
	nvxnu_work_cancel_wait(dl.wake_work);
	nvxnu_work_cancel_wait(dl.timer_work);
}

void tgm_deadline_resume(void)
{
	if (!READ_ONCE(dl.device))
		return;
	WRITE_ONCE(dl.recal, dl.recal + 1);
	WRITE_ONCE(dl.suspended, 0);
}

int tgm_deadline_counts(struct tgm_deadline_counts *c)
{
	if (!READ_ONCE(dl.device))
		return 0;
	c->frames = READ_ONCE(dl.frames);
	c->missed_app = READ_ONCE(dl.missed_app);
	c->missed_clear = READ_ONCE(dl.missed_clear);
	c->windows = READ_ONCE(dl.windows);
	c->empty = READ_ONCE(dl.empty);
	c->expired = READ_ONCE(dl.expired);
	c->kick_opens = READ_ONCE(dl.kick_opens);
	c->frame_end_runs = READ_ONCE(dl.frame_end_runs);
	c->timer_runs = READ_ONCE(dl.timer_runs);
	c->wake_runs = READ_ONCE(dl.wake_runs);
	c->expected = READ_ONCE(dl.expected);
	return 1;
}
