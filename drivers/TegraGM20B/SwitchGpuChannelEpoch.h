/* SPDX-License-Identifier: MIT */
/* The channel epoch (TegraGM20BABI.h TegraGM20BChannelEpochWord): one count for the GPU, its value in every connection's
 * fence page. It moves on at each event that gives back what a refused ChannelCreate lacked: a channel destroyed (its
 * id, its connection's slot, its ring and context), a connection closed (its memory), and pages given back to the GPU
 * pool after it refused one of the kernel's allocations. The count is gpu_chan_epoch.c's. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "TegraGM20BABI.h"

#ifdef __cplusplus
extern "C" {
#endif

/* gpu_chan_epoch.c: the next value into every connection's page; the current one into a page just handed out */
void switch_gpu_channel_epoch_move(void);
void switch_gpu_channel_epoch_fill(volatile uint32_t *page);
extern uint32_t switch_gpu_channel_memory_refused, switch_gpu_channel_creates;

static inline void switch_gpu_channel_epoch_store(volatile uint32_t *page, uint32_t value)
{
	__atomic_store_n(page + TegraGM20BChannelEpochWord, value, __ATOMIC_RELEASE);
}

/* The epoch's lock held. Several threads move it and a page's word must only move forward, so the next value and its
 * stores are one critical section. Each store is a release after what came back is back: a client that loads the new
 * value (acquire) and then asks finds it. `pages`: null before the fence range is made. */
static inline void switch_gpu_channel_epoch_mirror(uint32_t *epoch, uint8_t *pages, uint32_t count,
						   size_t stride)
{
	const uint32_t value = ++*epoch;
	for (uint32_t i = 0; pages && i < count; i++)
		switch_gpu_channel_epoch_store((volatile uint32_t *)(pages + i * stride), value);
}

/* The pool refused one of the kernel's own allocations (a channel's ring, context, page tables): the next pages given
 * back may let a ChannelCreate succeed. A BoAlloc short of ready pages is not one: it takes system pages instead. */
static inline void switch_gpu_channel_pool_refused(void)
{
	__atomic_store_n(&switch_gpu_channel_memory_refused, 1u, __ATOMIC_RELEASE);
}

/* A channel being made (tgm_chan_new). What a failed one's rollback gives back to the pool was free when it began, so
 * no pages given back move the epoch while any is under way, or each refused client's rollback would wake the others
 * (and itself) to ask again with nothing come back. Counted: creates on several connections overlap. */
static inline void switch_gpu_channel_create_begins(void)
{
	__atomic_add_fetch(&switch_gpu_channel_creates, 1u, __ATOMIC_ACQ_REL);
}

static inline void switch_gpu_channel_create_ends(void)
{
	__atomic_sub_fetch(&switch_gpu_channel_creates, 1u, __ATOMIC_ACQ_REL);
}

/* Pages given back to the GPU pool. With no refusal outstanding this is one relaxed load; the exchange gives a refusal's
 * one move to one of the frees that race for it. */
static inline void switch_gpu_channel_pool_given_back(void)
{
	if (__atomic_load_n(&switch_gpu_channel_memory_refused, __ATOMIC_RELAXED) &&
	    !__atomic_load_n(&switch_gpu_channel_creates, __ATOMIC_ACQUIRE) &&
	    __atomic_exchange_n(&switch_gpu_channel_memory_refused, 0u, __ATOMIC_ACQ_REL))
		switch_gpu_channel_epoch_move();
}

#ifdef __cplusplus
}
#endif
