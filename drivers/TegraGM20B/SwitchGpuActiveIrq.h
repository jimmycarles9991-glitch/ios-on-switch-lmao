/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

#define SWITCH_GPU_IRQ_ACTIVE (UINT64_C(1) << 32)

static inline int switch_gpu_irq_arm(uint64_t *state, uint32_t sequence)
{
	const uint64_t previous =
		__atomic_exchange_n(state, SWITCH_GPU_IRQ_ACTIVE | sequence, __ATOMIC_ACQ_REL);
	return !(previous & SWITCH_GPU_IRQ_ACTIVE);
}

static inline int switch_gpu_irq_complete(uint64_t *state, uint32_t completed)
{
	uint64_t seen = __atomic_load_n(state, __ATOMIC_ACQUIRE);
	if (!(seen & SWITCH_GPU_IRQ_ACTIVE))
		return 1;
	if ((int32_t)(completed - (uint32_t)seen) < 0)
		return 0;
	return __atomic_compare_exchange_n(state, &seen, (uint32_t)seen, 0, __ATOMIC_ACQ_REL,
					   __ATOMIC_ACQUIRE);
}

/* A channel's fence waiters, by the lowest seqno one of them sleeps for: each publishes its own before it tests the
 * fence, and the interrupt pass wakes them once the fence reaches the lowest published, clearing it, so a waiter still
 * short of its seqno publishes again before it sleeps. A seqno left by a waiter that gave up costs one early wake. */
#define SWITCH_GPU_WAITER_WANTED (UINT64_C(1) << 32)

static inline void switch_gpu_waiter_want(uint64_t *wanted, uint32_t seqno)
{
	uint64_t seen = __atomic_load_n(wanted, __ATOMIC_RELAXED);

	while (!((seen & SWITCH_GPU_WAITER_WANTED) && (int32_t)((uint32_t)seen - seqno) <= 0) &&
	       !__atomic_compare_exchange_n(wanted, &seen, SWITCH_GPU_WAITER_WANTED | seqno, 0,
					    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
		;
	/* Store-load order with the pass (fence read, then this word read): one of the two sees the other. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* The interrupt pass, with `completed` read from the fence word just before. */
static inline int switch_gpu_waiter_reached(uint64_t *wanted, uint32_t completed)
{
	uint64_t seen = 0;

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	seen = __atomic_load_n(wanted, __ATOMIC_RELAXED);
	if (!(seen & SWITCH_GPU_WAITER_WANTED) || (int32_t)(completed - (uint32_t)seen) < 0)
		return 0;
	__atomic_store_n(wanted, 0, __ATOMIC_RELAXED);
	return 1;
}
