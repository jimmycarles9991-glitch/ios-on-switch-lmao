// SPDX-License-Identifier: MIT
/* The deadline's stats lines (gpu_stats_tick.c, debug kernels): the deadline's and the hold's counts, and the app channels'
 * preemption latencies over as many lines as they take (deadline_stats.h TGM_DEADLINE_LINE). */
#include <core/os.h>
#include "TegraGM20BGpu.h"
#include "deadline_stats.h"

void tgm_deadline_format(char *buf, unsigned int len, const struct tgm_deadline_line *l)
{
	const struct tgm_deadline_counts *g = &l->gained;

	scnprintf(buf, len,
		  "%u compositor frames, %u missed their vblank with an app on the GPU at a "
		  "compositor kick, %u without; compositor GPU time a frame: mean %llu us, "
		  "p95 %llu us, max %llu us over %llu learned frames, peak tracker %llu us; "
		  "%u windows (%u with no frame, %u expired, %u opened by an app's kick); in the "
		  "work "
		  "loop's gate: %u frame ends, %u owner runs timed, %u woken",
		  g->frames, g->missed_app, g->missed_clear, l->mean_us, l->p95_us, l->max_us,
		  l->learned, g->expected / 1000, g->windows, g->empty, g->expired, g->kick_opens,
		  g->frame_end_runs, g->timer_runs, g->wake_runs);
}

void tgm_hold_format(char *buf, unsigned int len, const struct tgm_hold_counts *gained)
{
	scnprintf(buf, len,
		  "%u app channels held, %u early preempts (%u skipped: a preempt in flight or the "
		  "runlist busy), %u one-period turns, %u holds put off to the next vblank "
		  "(hopeless), "
		  "preemption latency %u samples, %u lowered",
		  gained->holds, gained->preempts, gained->busy, gained->turned, gained->hopeless,
		  gained->samples, gained->checks);
}

#if TGM_STATS
void tgm_latency_add(struct tgm_latency_lines *l, const char *owner, unsigned int chid,
		     uint64_t latency_ns, unsigned int turns)
{
	char entry[80];
	unsigned int n = 0;

	if (turns) {
		n = scnprintf(entry, sizeof(entry), " %s ch%u %llu us (%u turns)", owner, chid,
			      (unsigned long long)(latency_ns / 1000), turns);
	} else {
		n = scnprintf(entry, sizeof(entry), " %s ch%u %llu us", owner, chid,
			      (unsigned long long)(latency_ns / 1000));
	}
	if (l->at && l->at + n >= l->len) {
		l->print(l->buf);
		l->at = 0;
	}
	if (!l->at) {
		l->at = scnprintf(l->buf, l->len,
				  "latency by channel%s:", l->shown ? ", continued" : "");
	}
	l->at += scnprintf(l->buf + l->at, l->len - l->at, "%s", entry);
	l->shown++;
}

void tgm_latency_end(struct tgm_latency_lines *l)
{
	if (!l->shown)
		scnprintf(l->buf, l->len, "latency by channel: all at the minimum");
	l->print(l->buf);
}
#endif
