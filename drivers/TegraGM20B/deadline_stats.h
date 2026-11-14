/* SPDX-License-Identifier: MIT */
/* The deadline's and the hold's stats lines (deadline_stats.c, hold_stats.c) and what they read. The counts only grow,
 * written in the GPU interrupt work loop's gate; a line prints what they gained since the line before. */
#pragma once
#include <stdint.h>

struct tgm_deadline_counts {
	uint32_t frames, missed_app, missed_clear, windows, empty, expired;
	/* debug kernels: windows an app's kick opened; the gate's runs at a frame end, from the owner's timer, from a wake */
	uint32_t kick_opens, frame_end_runs, timer_runs, wake_runs;
	uint64_t expected;
};

/* 0 before the deadline is set up */
int tgm_deadline_counts(struct tgm_deadline_counts *c);
/* The gate, debug kernels: a frame's learned GPU time, for the line's mean, p95 and maximum */
void tgm_deadline_learned(uint64_t ns);

struct tgm_hold_counts {
	uint32_t holds, preempts, busy, turned, hopeless, samples, checks;
};

void tgm_hold_counts(struct tgm_hold_counts *c);
uint32_t tgm_hold_turns(unsigned int chid);
/* Debug kernels: the channel's process (its first submission's), and in the gate a preemption latency sampled for it */
void tgm_hold_owner(unsigned int chid);
void tgm_hold_landed(unsigned int chid, uint64_t asked_pt, uint64_t start_pt, uint64_t sample);
