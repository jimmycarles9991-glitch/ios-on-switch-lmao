/* SPDX-License-Identifier: MIT */
/*
 * GPU-ordered TLB invalidates for user raw maps (gpu_exec.c). A map writes its PTEs with no L2 or TLB maintenance on the CPU
 * and moves its address space's generation on. Each channel of that space remembers the generation it last
 * invalidated at, and a channel behind it starts its next submission with the host's L2 invalidate and then an MMU
 * TLB invalidate, so its walks see the new PTEs. A map only adds translations to pages its own process owns.
 * Everything that removes a translation (unmap, the unmap half of a rebind, a failed map's put, kernel mappings,
 * teardown) stays synchronous, so a stale entry never outlives the pages or page tables it named.
 */
#ifndef TEGRA_GM20B_TLB_H
#define TEGRA_GM20B_TLB_H
#include <stdint.h>

struct tgm_tlb_space {
	uint32_t gen; /* atomic */
};

struct tgm_tlb_chan {
	uint32_t seen; /* the channel's submissions are serialised */
};

/* A new space starts ahead of every new channel, so a channel's first submission always invalidates. */
static inline void tgm_tlb_space_init(struct tgm_tlb_space *s)
{
	__atomic_store_n(&s->gen, 1, __ATOMIC_RELAXED);
}

static inline void tgm_tlb_chan_init(struct tgm_tlb_chan *c)
{
	c->seen = 0;
}

/* After a map's PTE writes, before the map returns to its caller. */
static inline void tgm_tlb_mapped(struct tgm_tlb_space *s)
{
	__atomic_add_fetch(&s->gen, 1, __ATOMIC_RELEASE);
}

/* At the start of a submission: 1 when it must invalidate before its first push. */
static inline int tgm_tlb_take(struct tgm_tlb_chan *c, const struct tgm_tlb_space *s)
{
	const uint32_t gen = __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE);

	if (c->seen == gen)
		return 0;
	c->seen = gen;
	return 1;
}

#endif
