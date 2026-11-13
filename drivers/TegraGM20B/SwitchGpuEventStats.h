// SPDX-License-Identifier: MIT
// Debug kernels: each stats window's signals released mid-job into an event's word, how they completed, and the waits
// that acquired on a word: the periodic "events" line. A release kernel has the calls only, empty.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define SWITCH_GPU_EVENT_STATS 1
void switch_gpu_event_stats_released(uint32_t signals);
void switch_gpu_event_stats_settled(int by_word);
void switch_gpu_event_stats_waited(void);
void switch_gpu_event_stats_window(char *line, size_t size);
#else
#define SWITCH_GPU_EVENT_STATS 0

static inline void switch_gpu_event_stats_released(uint32_t signals)
{
	(void)signals;
}

static inline void switch_gpu_event_stats_settled(int by_word)
{
	(void)by_word;
}

static inline void switch_gpu_event_stats_waited(void)
{
}

static inline void switch_gpu_event_stats_window(char *line, size_t size)
{
	if (size)
		line[0] = 0;
}
#endif

#ifdef __cplusplus
}
#endif
