/* SPDX-License-Identifier: MIT */
/* The deadline window a frame end or a present kept for the first slow app channel's kick (deadline.c), in words with
 * one writer each: the frame end's (the display's work loop) and the present's and `done` (the GPU interrupt work
 * loop's gate: the latest vblank a window opened for or a present ended, which no kick opens again). A kept window's
 * vblank (0: none) is its sequence: cleared, its time, then the vblank released; a reader loads the vblank, the time,
 * and the vblank again. */
#pragma once
#include <stdint.h>

struct switch_gpu_kept_window {
	uint64_t frame_end, frame_end_since;
	uint64_t present, present_since;
	uint64_t done;
};

// NOLINTNEXTLINE(readability-non-const-parameter): the atomic stores below write through both
static inline void switch_gpu_kept_set(uint64_t *vblank, uint64_t *since, uint64_t target,
				       uint64_t at)
{
	__atomic_store_n(vblank, 0, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(since, at, __ATOMIC_RELAXED);
	__atomic_store_n(vblank, target, __ATOMIC_RELEASE);
}

static inline int switch_gpu_kept_any(const struct switch_gpu_kept_window *k)
{
	return __atomic_load_n(&k->frame_end, __ATOMIC_ACQUIRE) ||
	       __atomic_load_n(&k->present, __ATOMIC_ACQUIRE);
}

/* The gate */
static inline void switch_gpu_kept_done(struct switch_gpu_kept_window *k, uint64_t target)
{
	if (target > __atomic_load_n(&k->done, __ATOMIC_RELAXED)) {
		__atomic_store_n(&k->done, target, __ATOMIC_RELEASE);
	}
}

/* The kept window still to open at `now`: its vblank and *since, 0 if none. One older than `latest` (the latest frame
 * end's vblank) is gone: that frame end decided again, as is one a new frame end replaced between the loads. */
static inline uint64_t switch_gpu_kept_window(const struct switch_gpu_kept_window *k, uint64_t now,
					      uint64_t latest, uint64_t *since)
{
	const uint64_t done = __atomic_load_n(&k->done, __ATOMIC_ACQUIRE);
	const uint64_t present = __atomic_load_n(&k->present, __ATOMIC_ACQUIRE);
	const uint64_t frame_end = __atomic_load_n(&k->frame_end, __ATOMIC_ACQUIRE);
	const uint64_t target = frame_end > present ? frame_end : present;

	if (target <= now || target <= done || target < latest) {
		return 0;
	}
	if (target != frame_end) {
		*since = __atomic_load_n(&k->present_since, __ATOMIC_RELAXED);
		return target;
	}
	*since = __atomic_load_n(&k->frame_end_since, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&k->frame_end, __ATOMIC_RELAXED) == frame_end ? target : 0;
}
