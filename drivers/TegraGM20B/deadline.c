// SPDX-License-Identifier: MIT
/*
 * The compositor's deadline (TegraGM20BSched.h). The display sends Core Animation a vsync only while CA has vsync on (it
 * turns it on when it schedules an update, off after an idle countdown: QuartzCore IOMFBServer::set_vsync_enabled and
 * vsync_callback), and CA starts a frame at it for the vblank after the one it came with; the display passes each one on
 * here with its real vblank (tegra_gm20b_compositor_vsync). 24B119's CA never turns it on (IOMFBServer's use-VBL bit is
 * cleared at construction and nothing sets it; its frame clock is a CADisplayTimer), so there the display passes each
 * frame end on instead. A window then holds each app channel slow enough to be worth it (tgm_hold_worth) from its own
 * preemption latency before the compositor's latest start (that vblank less the compositor's GPU time per frame) until the
 * frame's present completes (hold.c); with no such channel no window opens. The GPU time per frame is learned from the high channels' completions (gpu_fenced.h
 * high_fenced), summed per present. QuartzCore's own hint (tegra_gm20b_display_deadline) comes once a second
 * (hint_agx_timing::next_agx_hint), too seldom to start a window from. Every decision is made at a vsync, a kick, a
 * fence interrupt or a host-time callout; nothing waits or polls.
 */
#include <core/device.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BSched.h"
#include "TegraGM20BSchedPolicy.h"
#include "TegraGM20BCostHist.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

static struct {
	spinlock_t lock;
	struct nvkm_device *device;
	void *hold_work, *check_work, *expire_work;
	u64 interval;    /* the frame interval */
	u64 next_target; /* the vblank the frame CA started at the last vsync aims for */
	u64 vsync_last, vsync_since; /* the last vsync to CA, and the start of its unbroken run */
	u64 kick_last;               /* the compositor's last kick */
	u64 expected;                /* the compositor's GPU time per frame */

	/* the window: the vblank it aims for (0: none), when it opened, whether the compositor kicked in it */
	u64 target, opened, expire_at;
	u32 gen, expire_gen;
	int kicked, suspended, check_chid;
	/* the frame since the last present: its GPU time, the vblank its first kick aimed for, an app on the GPU at a kick */
	u64 frame_ns, frame_target;
	int frame_open, frame_biased, frame_app;
	u32 frames, missed_app, missed_clear, windows, empty, expired;
	/* PTIMER against the kernel clock: an anchor of both (taken again after each GPU power-up), the rate (16.16) */
	u64 cal_ns, cal_pt, pt_rate;
} dl;

#if TGM_STATS
/* The stats window's learned frames: `dl.expected` is a peak tracker (tgm_expect), so the line sets them beside it. */
static struct {
	u32 hist[TGM_COST_BUCKETS];
	u64 n, sum, max;
} frame_cost;

static void frame_cost_add(u64 ns)
{
	frame_cost.hist[tgm_cost_bucket(ns)]++;
	frame_cost.n++;
	frame_cost.sum += ns;
	if (ns > frame_cost.max)
		frame_cost.max = ns;
}
#endif

static void hold_work(void *arg);
static void check_work(void *arg);
static void expire_work(void *arg);

void tgm_deadline_init(struct nvkm_device *device)
{
	if (dl.device)
		return;
	spin_lock_init(&dl.lock);
	dl.hold_work = nvxnu_work_alloc(hold_work, NULL);
	dl.check_work = nvxnu_work_alloc(check_work, NULL);
	dl.expire_work = nvxnu_work_alloc(expire_work, NULL);
	if (!dl.hold_work || !dl.check_work || !dl.expire_work)
		nvxnu_panic("tegra-gm20b: deadline: no thread calls");
	WRITE_ONCE(dl.device, device);
}

static int drawing(u64 now)
{
	return tgm_compositor_drawing(now, dl.interval, dl.vsync_last, dl.vsync_since,
				      dl.kick_last);
}

static void after(void *work, u64 at, u64 now)
{
	nvxnu_work_schedule_after(work, at > now ? at - now : 0);
}

/* dl.lock held: a window for the frame aiming at `target`; the holds come from hold_work, now and at each one's time */
static void window_open(u64 target, u64 now)
{
	dl.target = target;
	dl.opened = now;
	dl.kicked = 0;
	dl.expire_at = target;
	dl.expire_gen = ++dl.gen;
	if (TGM_STATS)
		dl.windows++;
}

/* dl.lock held */
static void window_close(u64 now)
{
	tgm_hold_release(dl.device, now);
	dl.target = 0;
}

void tegra_gm20b_compositor_vsync(uint64_t stamp, uint32_t lead_us, uint64_t interval)
{
	struct nvkm_device *device = READ_ONCE(dl.device);
	unsigned long flags = 0;
	u64 now = nvxnu_nanotime();
	u64 step = 0;
	u64 vblank = 0;
	u64 target = 0;

	if (!device)
		return;
	step = nvxnu_abstime_to_ns(interval);
	vblank = nvxnu_abstime_to_ns(stamp) + lead_us * 1000ULL;
	spin_lock_irqsave(&dl.lock, flags);
	if (!dl.vsync_last || now - dl.vsync_last > step * 3 / 2)
		dl.vsync_since = now;
	dl.vsync_last = now;
	dl.interval = step;
	dl.next_target = vblank + step;
	if (!dl.target && !dl.suspended && step && step <= TGM_INTERVAL_MAX_NS && drawing(now) &&
	    tgm_hold_wanted()) {
		window_open(dl.next_target, now);
		target = dl.target;
	}
	spin_unlock_irqrestore(&dl.lock, flags);
	if (target) {
		nvxnu_work_schedule(dl.hold_work);
		after(dl.expire_work, target, now);
	}
}

/* dl.lock held: a window is open and the GPU may be touched (system sleep cancels and waits for the callouts after it
 * sets suspended, and they look before they read a register) */
static int window_live(u64 now)
{
	return dl.target && !dl.suspended && drawing(now);
}

static void hold_work(void *arg)
{
	u64 now = nvxnu_nanotime();
	u64 next = 0;
	int resident = 0;
	int preempt = -1;
	int live = 0;
	struct tgm_window w;
	unsigned long flags = 0;

	(void)arg;
	spin_lock_irqsave(&dl.lock, flags);
	live = window_live(now);
	spin_unlock_irqrestore(&dl.lock, flags);
	if (!live)
		return;
	resident = tgm_sched_resident(dl.device);
	spin_lock_irqsave(&dl.lock, flags);
	if (window_live(now)) {
		w.now = now;
		w.vblank = dl.target;
		w.interval = dl.interval;
		w.expected = dl.expected;
		next = tgm_hold_due(dl.device, &w, resident, &preempt);
	}
	spin_unlock_irqrestore(&dl.lock, flags);
	if (preempt >= 0 && tgm_hold_preempt(dl.device, preempt)) {
		WRITE_ONCE(dl.check_chid, preempt);
		nvxnu_work_schedule_after(dl.check_work, tgm_hold_latency(preempt) / 2);
	}
	if (next)
		after(dl.hold_work, next, now);
}

static void check_work(void *arg)
{
	(void)arg;
	if (!READ_ONCE(dl.suspended))
		tgm_hold_check(dl.device, READ_ONCE(dl.check_chid));
}

/* At the window's vblank: a window the compositor never kicked in had no frame (CA drew nothing at that vsync) and ends;
 * one with a frame still in flight gets one more interval, then ends. */
static void expire_work(void *arg)
{
	u64 now = nvxnu_nanotime();
	unsigned long flags = 0;
	int again = 0;

	(void)arg;
	spin_lock_irqsave(&dl.lock, flags);
	if (dl.target && dl.expire_gen == dl.gen) {
		if (dl.kicked && dl.expire_at == dl.target) {
			dl.expire_at = dl.target + dl.interval;
			again = 1;
		} else {
			if (TGM_STATS && dl.kicked) {
				dl.expired++;
			} else if (TGM_STATS) {
				dl.empty++;
			}
			window_close(now);
		}
	}
	spin_unlock_irqrestore(&dl.lock, flags);
	if (again)
		after(dl.expire_work, dl.expire_at, now);
}

void tgm_deadline_kicked(int compositor, int app)
{
	u64 now = nvxnu_nanotime();
	unsigned long flags = 0;

	if (!READ_ONCE(dl.device))
		return;
	spin_lock_irqsave(&dl.lock, flags);
	if (compositor) {
		dl.kick_last = now;
		if (dl.target)
			dl.kicked = 1;
		if (!dl.frame_open) {
			dl.frame_open = 1;
			dl.frame_target = dl.next_target;
		}
	}
	if (app)
		dl.frame_app = 1;
	spin_unlock_irqrestore(&dl.lock, flags);
}

/* Interrupt pass. The anchor moves on every 2^40 ns (18 minutes), keeping the rate, so the span never overflows. */
void tgm_deadline_clock(uint64_t ns, uint64_t pt)
{
	unsigned long flags = 0;
	u64 rate = 0;

	spin_lock_irqsave(&dl.lock, flags);
	if (!dl.cal_ns || pt < dl.cal_pt || ns - dl.cal_ns >= 1ULL << 40) {
		dl.cal_ns = ns;
		dl.cal_pt = pt;
	} else {
		rate = tgm_pt_rate(ns - dl.cal_ns, pt - dl.cal_pt);
		if (rate)
			WRITE_ONCE(dl.pt_rate, rate);
	}
	spin_unlock_irqrestore(&dl.lock, flags);
}

uint64_t tgm_deadline_pt_ns(uint64_t pt_span)
{
	return tgm_pt_ns(pt_span, READ_ONCE(dl.pt_rate));
}

void tgm_deadline_progress(uint64_t ns, int biased)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&dl.lock, flags);
	dl.frame_ns += ns;
	if (biased)
		dl.frame_biased = 1;
	spin_unlock_irqrestore(&dl.lock, flags);
}

/* The present completed: the frame's GPU time is learned (unless an app held the GPU at one of its kicks), and the window
 * whose frame this is ends (a present queued before it opened is an earlier frame's). If CA has started the next frame
 * since, its window opens now. */
void tgm_deadline_presented(uint64_t queued)
{
	u64 now = nvxnu_nanotime();
	u64 target = 0;
	unsigned long flags = 0;

	if (!READ_ONCE(dl.device))
		return;
	spin_lock_irqsave(&dl.lock, flags);
	if (dl.frame_open) {
		if (TGM_STATS) {
			dl.frames++;
			if (dl.frame_target && now > dl.frame_target && dl.frame_app) {
				dl.missed_app++;
			} else if (dl.frame_target && now > dl.frame_target) {
				dl.missed_clear++;
			}
		}
		if (!dl.frame_biased && dl.frame_ns) {
			dl.expected = tgm_expect(dl.expected, dl.frame_ns);
#if TGM_STATS
			frame_cost_add(dl.frame_ns);
#endif
		}
	}
	dl.frame_open = dl.frame_biased = dl.frame_app = 0;
	dl.frame_ns = 0;
	tgm_hold_unneed(queued);
	if (dl.target && queued >= dl.opened) {
		u64 closed = dl.target;

		window_close(now);
		if (!dl.suspended && dl.next_target > closed && dl.next_target > now &&
		    drawing(now) && tgm_hold_wanted()) {
			window_open(dl.next_target, now);
			target = dl.target;
		}
	}
	spin_unlock_irqrestore(&dl.lock, flags);
	if (target) {
		nvxnu_work_schedule(dl.hold_work);
		after(dl.expire_work, target, now);
	}
}

void tgm_deadline_suspend(void)
{
	unsigned long flags = 0;

	if (!READ_ONCE(dl.device))
		return;
	spin_lock_irqsave(&dl.lock, flags);
	dl.suspended = 1;
	if (dl.target)
		window_close(nvxnu_nanotime());
	spin_unlock_irqrestore(&dl.lock, flags);
	nvxnu_work_cancel_wait(dl.hold_work);
	nvxnu_work_cancel_wait(dl.check_work);
	nvxnu_work_cancel_wait(dl.expire_work);
}

void tgm_deadline_resume(void)
{
	unsigned long flags = 0;

	if (!READ_ONCE(dl.device))
		return;
	spin_lock_irqsave(&dl.lock, flags);
	dl.suspended = 0;
	dl.cal_ns = 0;
	spin_unlock_irqrestore(&dl.lock, flags);
}

void tgm_deadline_stats(char *buf, unsigned int len)
{
	u32 frames = 0;
	u32 missed_app = 0;
	u32 missed_clear = 0;
	u32 windows = 0;
	u32 empty = 0;
	u32 expired = 0;
	unsigned long flags = 0;
	u64 expected = 0;
	u64 learned = 0;
	u64 mean = 0;
	u64 p95 = 0;
	u64 max = 0;

	if (!READ_ONCE(dl.device)) {
		scnprintf(buf, len, "deadline off");
		return;
	}
	spin_lock_irqsave(&dl.lock, flags);
	frames = dl.frames;
	missed_app = dl.missed_app;
	missed_clear = dl.missed_clear;
	windows = dl.windows;
	empty = dl.empty;
	expired = dl.expired;
	expected = dl.expected;
	dl.frames = dl.missed_app = dl.missed_clear = dl.windows = dl.empty = dl.expired = 0;
#if TGM_STATS
	learned = frame_cost.n;
	mean = learned ? frame_cost.sum / learned / 1000 : 0;
	p95 = tgm_cost_percentile(frame_cost.hist, learned, 950);
	max = frame_cost.max / 1000;
	memset(&frame_cost, 0, sizeof(frame_cost));
#endif
	spin_unlock_irqrestore(&dl.lock, flags);
	scnprintf(buf, len,
		  "%u compositor frames, %u missed their vblank with an app on the GPU at a "
		  "compositor kick, %u without; compositor GPU time a frame: mean %llu us, "
		  "p95 %llu us, max %llu us over %llu learned frames, peak tracker %llu us; "
		  "%u windows (%u with no frame, %u expired)",
		  frames, missed_app, missed_clear, mean, p95, max, learned, expected / 1000,
		  windows, empty, expired);
}
