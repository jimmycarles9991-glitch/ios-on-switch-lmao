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

#if TGM_STATS
static const char *const kMissCause[TGM_MISS_CAUSES] = {
	"an app's job held the GPU at a compositor kick",
	"the compositor's own GPU time past what it learned",
	"the compositor's first kick too late for its GPU time",
	"an app's work on the GPU between the compositor's kicks",
	"the present completed late with GPU time to spare",
};

/* The gate's counts, which only grow, and the line budget of the window it last saw; the tick's window and what its
 * last line read */
static struct {
	u32 by_cause[TGM_MISS_CAUSES];
	u32 lines, lines_window;
	u32 window;
	u32 seen[TGM_MISS_CAUSES];
} miss;
#endif

void tgm_deadline_missed(const struct tgm_deadline_miss *m)
{
#if TGM_STATS
	const enum tgm_deadline_miss_cause cause = tgm_deadline_miss_cause(m);
	const u32 window = READ_ONCE(miss.window);
	char line[320];

	WRITE_ONCE(miss.by_cause[cause], miss.by_cause[cause] + 1);
	if (miss.lines_window != window) {
		miss.lines_window = window;
		miss.lines = 0;
	}
	if (++miss.lines > 8)
		return;
	/* formatted here and logged as one string: os_log drops a message whose scalars outgrow its buffer */
	scnprintf(line, sizeof(line),
		  "tegra-gm20b: compositor frame missed its vblank by %llu us: %s (its GPU time "
		  "%llu us, learned %llu "
		  "us; first kick %llu us before the vblank; an app kicked %d, held the GPU %d)",
		  m->late_ns / 1000, kMissCause[cause], m->gpu_ns / 1000, m->expected_ns / 1000,
		  m->kick_lead_ns / 1000, m->app_kicked, m->app_held);
	nvxnu_log("%s", line);
#else
	(void)m;
#endif
}

int tgm_deadline_miss_stats(char *buf, unsigned int len)
{
#if TGM_STATS
	u32 now[TGM_MISS_CAUSES];
	u32 total = 0;
	unsigned int i = 0;

	for (i = 0; i < TGM_MISS_CAUSES; i++) {
		now[i] = READ_ONCE(miss.by_cause[i]) - miss.seen[i];
		miss.seen[i] += now[i];
		total += now[i];
	}
	WRITE_ONCE(miss.window, miss.window + 1);
	if (!total)
		return 0;
	scnprintf(buf, len,
		  "%u compositor frames missed their vblank: app held the GPU %u, own GPU time %u, "
		  "first kick late %u, "
		  "app work between kicks %u, present late %u",
		  total, now[TGM_MISS_APP_HELD], now[TGM_MISS_OWN_GPU], now[TGM_MISS_LATE_KICK],
		  now[TGM_MISS_APP_BETWEEN], now[TGM_MISS_PRESENT]);
	return 1;
#else
	(void)buf;
	(void)len;
	return 0;
#endif
}

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
	struct tgm_deadline_line line;
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
	line = (struct tgm_deadline_line){
		.gained = {.frames = now.frames - was->frames,
			   .missed_app = now.missed_app - was->missed_app,
			   .missed_clear = now.missed_clear - was->missed_clear,
			   .windows = now.windows - was->windows,
			   .empty = now.empty - was->empty,
			   .expired = now.expired - was->expired,
			   .kick_opens = now.kick_opens - was->kick_opens,
			   .frame_end_runs = now.frame_end_runs - was->frame_end_runs,
			   .timer_runs = now.timer_runs - was->timer_runs,
			   .wake_runs = now.wake_runs - was->wake_runs,
			   .expected = now.expected},
		.mean_us = mean,
		.p95_us = p95,
		.max_us = max,
		.learned = learned,
	};
	tgm_deadline_format(buf, len, &line);
	*was = now;
}
