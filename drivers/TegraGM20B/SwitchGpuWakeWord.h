/* SPDX-License-Identifier: MIT */
/* The global wake word (nvxnu_wake, nvxnu_idle) as two sequences a sleeper reads as their sum: one only the interrupt
 * pass moves, with a plain store (it is the pass's alone), and one every other waker moves. A pass wakes a thread only
 * while one sleeps: a sleeper is counted before it looks at the sequences, and a waker looks at the count after it moved
 * its sequence, so a sleeper either sees the move or is seen. */
#pragma once
#include <stdint.h>

struct switch_gpu_wake_word {
	uint32_t pass;
	uint32_t others;
	uint32_t sleepers;
};

static inline uint32_t switch_gpu_wake_seq(const struct switch_gpu_wake_word *w)
{
	return __atomic_load_n(&w->pass, __ATOMIC_ACQUIRE) +
	       __atomic_load_n(&w->others, __ATOMIC_ACQUIRE);
}

/* The interrupt pass, after it made a sleeper's condition true: 1 when a thread may sleep on the word, which the caller
 * then wakes. */
static inline int switch_gpu_wake_pass(struct switch_gpu_wake_word *w)
{
	__atomic_store_n(&w->pass, __atomic_load_n(&w->pass, __ATOMIC_RELAXED) + 1u,
			 __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return __atomic_load_n(&w->sleepers, __ATOMIC_RELAXED) != 0u ? 1 : 0;
}

/* Any other waker (a work item's wake_up or complete, a channel's kill) */
static inline int switch_gpu_wake_other(struct switch_gpu_wake_word *w)
{
	__atomic_fetch_add(&w->others, 1u, __ATOMIC_SEQ_CST);
	return __atomic_load_n(&w->sleepers, __ATOMIC_SEQ_CST) != 0u ? 1 : 0;
}

/* Around a sleep on the word: on before it reads the sequences, off once it is awake. */
static inline void switch_gpu_wake_sleep(struct switch_gpu_wake_word *w, int on)
{
	if (on != 0) {
		__atomic_fetch_add(&w->sleepers, 1u, __ATOMIC_SEQ_CST);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
	} else {
		__atomic_fetch_sub(&w->sleepers, 1u, __ATOMIC_RELEASE);
	}
}
