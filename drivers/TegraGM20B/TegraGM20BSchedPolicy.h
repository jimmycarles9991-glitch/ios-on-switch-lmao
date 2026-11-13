/* SPDX-License-Identifier: MIT */
/* The compositor priority's decisions (sched.c, deadline.c), apart from the registers and nvkm's lists, so a host test
 * runs them. Times are nanoseconds on the kernel's clock (nvxnu_nanotime). */
#ifndef TEGRA_GM20B_SCHED_POLICY_H
#define TEGRA_GM20B_SCHED_POLICY_H
#include <stdint.h>
#include "TegraGM20BHost.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TGM_VICTIM_NONE (-1)
#define TGM_VICTIM_SWITCHING (-2)

/* A channel's runlist level, lowest first: runningboardd's background and foreground roles, and the compositor's. */
enum tgm_level {
	TGM_LEVEL_BACKGROUND,
	TGM_LEVEL_FOREGROUND,
	TGM_LEVEL_HIGH,
	TGM_LEVELS,
};

/* A connection's runlist level from the GPU priority its Metal queues asked for (TegraGM20BABI.h method 18) and whether
 * runningboardd denies its process the GPU (`background`: then its background priority counts). */
static inline enum tgm_level tgm_priority_level(uint32_t priority, uint32_t background_priority,
						int background)
{
	const uint32_t p = background ? background_priority : priority;

	if (p == 0 && !background)
		return TGM_LEVEL_HIGH;
	return p <= 2 ? TGM_LEVEL_FOREGROUND : TGM_LEVEL_BACKGROUND;
}

/* What a high channel's kick should preempt, from engine 0's (GR's) status, whether its resident channel is high, and the
 * PBDMA's: the app channel holding GR, else the one holding the PBDMA; TGM_VICTIM_SWITCHING while GR is mid-switch (the
 * host already leaves a channel, and with the runlist interleaved goes to a high one next). A TSG id names no channel. */
static inline int tgm_kick_victim(uint32_t eng, int eng_high, int pbdma_busy, uint32_t pbdma_chid,
				  int pbdma_high)
{
	if (eng & PFIFO_ENG_BUSY) {
		if (eng & PFIFO_ENG_CHSW)
			return TGM_VICTIM_SWITCHING;
		if (PFIFO_ENG_CTX_STATUS(eng) == PFIFO_ENG_CTX_VALID && !(eng & PFIFO_ENG_ID_TSG) &&
		    !eng_high)
			return (int)PFIFO_ENG_ID(eng);
	}
	if (pbdma_busy && !pbdma_high)
		return (int)PBDMA_CHID(pbdma_chid);
	return TGM_VICTIM_NONE;
}

/* While GR is mid-switch, what a high kick preempts: the app channel GR is loading (a load, or a save and a load), which
 * the host picked while no high channel had work and which would keep GR for its timeslice; TGM_VICTIM_SWITCHING for a
 * save alone or a switch to a high channel or a TSG, which the interleaved runlist follows with a high channel. */
static inline int tgm_kick_loading(uint32_t eng, int next_high)
{
	const uint32_t ctx = PFIFO_ENG_CTX_STATUS(eng);

	if ((eng & PFIFO_ENG_BUSY) && (eng & PFIFO_ENG_CHSW) &&
	    (ctx == PFIFO_ENG_CTX_LOAD || ctx == PFIFO_ENG_CTX_SWITCH) &&
	    !(eng & PFIFO_ENG_NEXT_ID_TSG) && !next_high)
		return (int)PFIFO_ENG_NEXT_ID(eng);
	return TGM_VICTIM_SWITCHING;
}

/* Whether a high kick preempts its victim: never one high work waits for (the present's waits, the compositor's acquires,
 * tgm_hold_need): the compositor would only yield at its acquire and the host switch back. */
static inline int tgm_kick_may_preempt(int victim, int needed)
{
	return (victim >= 0 && needed == 0) ? 1 : 0;
}

static inline void tgm_interleave_units(const uint8_t *tier_of, unsigned units, unsigned tier,
					uint16_t *order, unsigned *n)
{
	unsigned i = 0;

	for (i = 0; i < units; i++) {
		if (tier_of[i] == tier)
			order[(*n)++] = (uint16_t)i;
	}
}

/* Appends the tiers above the bottom one (two or three tiers in all): the top tier, and with a middle tier, each of its
 * units followed by the top tier again (nvgpu gk20a_runlist_construct_locked). */
static inline void tgm_interleave_upper(const uint8_t *tier_of, unsigned units, unsigned tiers,
					uint16_t *order, unsigned *n)
{
	unsigned i = 0;

	tgm_interleave_units(tier_of, units, tiers - 1, order, n);
	if (tiers < 3)
		return;
	for (i = 0; i < units; i++) {
		if (tier_of[i] != 1)
			continue;
		order[(*n)++] = (uint16_t)i;
		tgm_interleave_units(tier_of, units, 2, order, n);
	}
}

/* Lays the units out in tiers, lowest first: one per level present, each bottom-tier unit followed by the tiers above.
 * `merge`: every level below the top one is one tier, and if that does not fit max entries and order_max units, the top
 * tier follows every `every`-th bottom unit only. 0 if there is a single tier or it does not fit. */
static inline unsigned tgm_interleave_tiers(const uint8_t *level, const uint16_t *entries,
					    unsigned units, int merge, unsigned max,
					    uint16_t *order, unsigned order_max)
{
	uint8_t tier_of_level[TGM_LEVELS] = {0};
	uint8_t tier_of[512];
	unsigned count[TGM_LEVELS] = {0};
	unsigned present[TGM_LEVELS] = {0};
	unsigned tiers = 0;
	unsigned every = 0;
	unsigned seen = 0;
	unsigned n = 0;
	unsigned i = 0;
	unsigned top = 0;
	uint64_t weight[TGM_LEVELS] = {0};
	uint64_t above_units = 0;
	uint64_t above_entries = 0;
	uint64_t repeats = 0;

	if (units > sizeof(tier_of))
		return 0;
	for (i = 0; i < units; i++) {
		present[level[i]] = 1;
		top = level[i] > top ? level[i] : top;
	}
	for (i = 0; i < TGM_LEVELS; i++) {
		if (!present[i])
			continue;
		if (merge && i < top) {
			tiers = 1;
		} else {
			tier_of_level[i] = (uint8_t)tiers++;
		}
	}
	if (tiers < 2)
		return 0;
	for (i = 0; i < units; i++) {
		tier_of[i] = tier_of_level[level[i]];
		count[tier_of[i]]++;
		weight[tier_of[i]] += entries[i];
	}
	above_units = count[tiers - 1];
	above_entries = weight[tiers - 1];
	if (tiers == 3) {
		above_units += (uint64_t)count[1] * (1 + count[2]);
		above_entries += weight[1] + count[1] * weight[2];
	}
	for (every = 1; every <= (merge ? count[0] : 1); every++) {
		repeats = count[0] / every;
		if (weight[0] + repeats * above_entries <= max &&
		    count[0] + repeats * above_units <= order_max)
			break;
	}
	if (every > (merge ? count[0] : 1))
		return 0;
	for (i = 0; i < units; i++) {
		if (tier_of[i])
			continue;
		order[n++] = (uint16_t)i;
		if (!(++seen % every))
			tgm_interleave_upper(tier_of, units, tiers, order, &n);
	}
	return n;
}

/* The runlist's order of units (a channel, or a channel group with its header) from their levels, the nvgpu way: every
 * unit of a level is followed by the levels above it, so whichever unit the host leaves, a higher one comes next, and
 * the compositor's units follow every other one. entries[i] is unit i's runlist entries; when that would pass max
 * entries, every level below the top one is laid out as one (the compositor still follows every app), and then the top
 * level follows every `every`-th of them only. With one level, or no room, each unit once, in order. Writes unit
 * indices to order (room for order_max) and returns how many. */
static inline unsigned tgm_interleave(const uint8_t *level, const uint16_t *entries, unsigned units,
				      unsigned max, uint16_t *order, unsigned order_max)
{
	unsigned n = 0;
	unsigned i = 0;

	n = tgm_interleave_tiers(level, entries, units, 0, max, order, order_max);
	if (!n)
		n = tgm_interleave_tiers(level, entries, units, 1, max, order, order_max);
	if (n)
		return n;
	for (i = 0; i < units && n < order_max; i++)
		order[n++] = (uint16_t)i;
	return n;
}

/* The deadline: the display's vsyncs to Core Animation give the vblank each frame aims for and the interval. */
#define TGM_INTERVAL_MAX_NS (100ull * 1000 * 1000)

/* Whether the compositor is drawing: CA has vsync on (one came within an interval and a half) and either turned it on just
 * now (the first frame after an idle) or kicked GPU work in the last two intervals; after its last frame, the vsyncs of its
 * idle countdown hold nothing. */
static inline int tgm_compositor_drawing(uint64_t now, uint64_t interval, uint64_t vsync_last,
					 uint64_t vsync_since, uint64_t kick_last)
{
	if (!vsync_last || !interval || now - vsync_last > interval * 3 / 2)
		return 0;
	return (now - vsync_since <= 2 * interval ||
		(kick_last != 0 && now - kick_last <= 2 * interval))
		       ? 1
		       : 0;
}

/* When an app channel is held off the GPU (disabled, and preempted if it is on it) so the compositor's frame, `expected`
 * of GPU time, makes `vblank`: one preemption latency of that channel (Maxwell leaves a draw only at its next
 * wait-for-idle) before the compositor's latest start; at or before now, hold it now. Past that time, the channel on the
 * GPU (`resident`) still needs its latency to leave and one not on it only needs disabling: if even that cannot make the
 * vblank, the frame lands a vblank later at best, and the hold waits for that one's time unless there is no slack. */
static inline uint64_t tgm_hold_time(uint64_t vblank, uint64_t interval, uint64_t now,
				     uint64_t expected, uint64_t preempt, int resident)
{
	const uint64_t lead = expected + preempt;

	if (vblank > lead && now < vblank - lead)
		return vblank - lead;
	if (now + expected + (resident ? preempt : 0) <= vblank)
		return now;
	if (vblank + interval > now + lead)
		return vblank + interval - lead;
	return now;
}

/* PTIMER counts at its own rate (deploy logs: 1.627 ns a unit): the kernel clock's nanoseconds per PTIMER unit in 16.16
 * fixed point over a span of both clocks (0 until the span is long enough to tell), and a PTIMER span in nanoseconds. */
#define TGM_PT_CAL_MIN_NS (100ull * 1000 * 1000)

static inline uint64_t tgm_pt_rate(uint64_t ns_span, uint64_t pt_span)
{
	if (ns_span < TGM_PT_CAL_MIN_NS || !pt_span || ns_span >= 1ULL << 47)
		return 0;
	return (ns_span << 16) / pt_span;
}

static inline uint64_t tgm_pt_ns(uint64_t pt_span, uint64_t rate)
{
	return (pt_span * rate) >> 16;
}

/* The GPU time of high work that ended at `end` and started at `start` (its start stamp, or its kick), less what overlapped
 * the high work that ended before it at `last_end`: summed per frame, the compositor's GPU time. */
static inline uint64_t tgm_gpu_span(uint64_t end, uint64_t start, uint64_t last_end)
{
	const uint64_t from = start > last_end ? start : last_end;

	return end > from ? end - from : 0;
}

/* Which channels a window holds; with none tracked no window opens. One whose preemption latency stays within the
 * slack is left to the compositor's kick, whose preempt then costs the compositor at most that: holding it every frame
 * would only take GPU time from it. One whose latency exceeds a 60 Hz frame cannot leave in time for any hold, and its
 * learned latency is mostly the compositor waiting on something else after the ask (F-v1: SwitchHWTest, mediaanalysisd
 * and the kernel's own channels at the 100 ms cap): holding it every frame would starve it for nothing. */
#define TGM_HOLD_SLACK_NS (1000ull * 1000)
#define TGM_HOLD_CEILING_NS (16666667ull)

/* The longest an app channel's draw is estimated to keep GR (drivers/TegraGM20BMetal raw_draw_bands.h): a longer one is
 * drawn as scissor bands, each ending a batch of its own (its wait-for-idle seqno release), so a compositor kick waits
 * for one band at most. The hold's slack: a channel whose draws fit it is never worth a hold. */
#define TGM_APP_BAND_NS TGM_HOLD_SLACK_NS

static inline int tgm_hold_worth(uint64_t preempt)
{
	return (preempt > TGM_HOLD_SLACK_NS && preempt <= TGM_HOLD_CEILING_NS) ? 1 : 0;
}

/* The one-period bound: a channel held in the last window that has not had its preemption latency's worth of GPU time
 * since that window closed is left alone this window, so an app whose draws outlast the compositor's slack still gets a
 * turn every other frame. */
static inline int tgm_owed_turn(int held_last, uint64_t since_close, uint64_t preempt)
{
	return (held_last != 0 && since_close < preempt) ? 1 : 0;
}

/* How a channel preempt nvkm asks for goes out (sched.c): a PFIFO_PREEMPT written while another is in flight is lost.
 * nvkm's error path asks from the interrupt pass with interrupts off under the channel's spinlock (nvkm_chan_error),
 * where nothing may wait: with one in flight there the channel's group goes to nvkm's recovery, which takes it off the
 * engines and the runlist from its work item. */
enum tgm_preempt_how {
	TGM_PREEMPT_NOW,
	TGM_PREEMPT_AFTER_WAIT,
	TGM_PREEMPT_RECOVER,
};

static inline enum tgm_preempt_how tgm_preempt_how(int irqs_off, int pending)
{
	if (pending == 0)
		return TGM_PREEMPT_NOW;
	return irqs_off != 0 ? TGM_PREEMPT_RECOVER : TGM_PREEMPT_AFTER_WAIT;
}

/* nvkm's recovery uses the same enable bits as the hold (nvkm_chan_error blocks a channel for good, nvkm_runl_rc_cgrp
 * puts its group in recovery): a channel errored, being isolated (gpu_hang.c's client fault isolation) or in a group under
 * recovery is never held or released here. */
static inline int tgm_may_release(int errored, int isolating, int rc)
{
	return (errored == 0 && isolating == 0 && rc == 0) ? 1 : 0;
}

/* An app channel the hold may take: below the compositor's level, registered by gpu_chan.c, not waited for by the present in
 * flight, and not already blocked by nvkm (a teardown or a context bind holds it itself). */
static inline int tgm_may_hold(unsigned level, int holdable, int needed, int errored, int isolating,
			       int rc, int blocked)
{
	return (level < TGM_LEVEL_HIGH && holdable != 0 && needed == 0 && blocked == 0 &&
		tgm_may_release(errored, isolating, rc) != 0)
		       ? 1
		       : 0;
}

/* The compositor's GPU time per frame: rises at once toward a longer frame, falls slowly, so one short frame does not
 * open the next window late. */
static inline uint64_t tgm_expect(uint64_t expected, uint64_t sample)
{
	if (sample > expected)
		return expected + (sample - expected + 1) / 2;
	return expected - (expected - sample) / 8;
}

/* A channel's preemption latency (preempt to its context switched out): an exact sample (the compositor's start stamp
 * after it) raises it at once, a shorter one lowers it by an eighth of the difference; never below min or above max. */
#define TGM_PREEMPT_MIN_NS (100ull * 1000)

static inline uint64_t tgm_preempt_sampled(uint64_t preempt, uint64_t sample, uint64_t max)
{
	if (sample > preempt) {
		preempt = sample;
	} else {
		preempt -= (preempt - sample) / 8;
	}
	if (preempt > max)
		preempt = max;
	return preempt < TGM_PREEMPT_MIN_NS ? TGM_PREEMPT_MIN_NS : preempt;
}

/* The check half its preemption latency after a preempt found the channel gone: lower it by an eighth. */
static inline uint64_t tgm_preempt_left_early(uint64_t preempt)
{
	preempt -= preempt / 8;
	return preempt < TGM_PREEMPT_MIN_NS ? TGM_PREEMPT_MIN_NS : preempt;
}

/* A stuck timer's time (now - since) less what the hold kept the channel off the GPU in it (held_now - held_then, the
 * channel's cumulative held time then and now). */
static inline uint64_t tgm_unheld_ns(uint64_t now, uint64_t since, uint64_t held_now,
				     uint64_t held_then)
{
	const uint64_t elapsed = now > since ? now - since : 0;
	const uint64_t held = held_now - held_then;

	return elapsed > held ? elapsed - held : 0;
}

#ifdef __cplusplus
}
#endif
#endif
