// SPDX-License-Identifier: MIT
/* The deadline's stats line. Debug kernels add the learned frames' GPU time: `expected` is a peak tracker
 * (tgm_expect), so the line sets them beside it. One caller at a time (the window's winner, tgm_stats_tick). */
#include <core/device.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BSched.h"
#include "TegraGM20BCostHist.h"
#include "deadline_stats.h"

#if TGM_STATS
/* The gate's: the histogram and sums only grow; the maximum is the window's the line last asked for */
static struct {
	u32 hist[TGM_COST_BUCKETS];
	u64 n, sum, max;
	u32 max_window, window;
} cost;
#endif

void tgm_deadline_learned(uint64_t ns)
{
#if TGM_STATS
	const u32 window = READ_ONCE(cost.window);
	const u32 bucket = tgm_cost_bucket(ns);

	WRITE_ONCE(cost.hist[bucket], cost.hist[bucket] + 1);
	WRITE_ONCE(cost.n, cost.n + 1);
	WRITE_ONCE(cost.sum, cost.sum + ns);
	if (cost.max_window != window) {
		cost.max_window = window;
		WRITE_ONCE(cost.max, 0);
	}
	if (ns > cost.max)
		WRITE_ONCE(cost.max, ns);
#else
	(void)ns;
#endif
}

static struct {
	struct tgm_deadline_counts counts;
#if TGM_STATS
	u32 hist[TGM_COST_BUCKETS];
	u64 n, sum;
#endif
} seen;

void tgm_deadline_stats(char *buf, unsigned int len)
{
	struct tgm_deadline_counts now;
	struct tgm_deadline_counts *was = &seen.counts;
	u64 learned = 0;
	u64 mean = 0;
	u64 p95 = 0;
	u64 max = 0;

	if (!tgm_deadline_counts(&now)) {
		scnprintf(buf, len, "deadline off");
		return;
	}
#if TGM_STATS
	{
		u32 hist[TGM_COST_BUCKETS];
		u64 sum = 0;
		unsigned int i = 0;

		for (i = 0; i < TGM_COST_BUCKETS; i++) {
			const u32 n = READ_ONCE(cost.hist[i]);

			hist[i] = n - seen.hist[i];
			seen.hist[i] = n;
		}
		learned = READ_ONCE(cost.n) - seen.n;
		seen.n += learned;
		sum = READ_ONCE(cost.sum) - seen.sum;
		seen.sum += sum;
		mean = learned ? sum / learned / 1000 : 0;
		p95 = tgm_cost_percentile(hist, learned, 950);
		max = READ_ONCE(cost.max) / 1000;
		WRITE_ONCE(cost.window, cost.window + 1);
	}
#endif
	scnprintf(buf, len,
		  "%u compositor frames, %u missed their vblank with an app on the GPU at a "
		  "compositor kick, %u without; compositor GPU time a frame: mean %llu us, "
		  "p95 %llu us, max %llu us over %llu learned frames, peak tracker %llu us; "
		  "%u windows (%u with no frame, %u expired, %u opened by an app's kick); in the "
		  "work "
		  "loop's gate: %u frame ends, %u owner runs timed, %u woken",
		  now.frames - was->frames, now.missed_app - was->missed_app,
		  now.missed_clear - was->missed_clear, mean, p95, max, learned,
		  now.expected / 1000, now.windows - was->windows, now.empty - was->empty,
		  now.expired - was->expired, now.kick_opens - was->kick_opens,
		  now.frame_end_runs - was->frame_end_runs, now.timer_runs - was->timer_runs,
		  now.wake_runs - was->wake_runs);
	*was = now;
}
