// SPDX-License-Identifier: MIT
#pragma once

/* In seqnos, from boot: a kick adds every seqno its job releases (its internal fences' and its own), and the nonstall
 * pass and channel teardown retire each one once. */
static inline u64 idle_in_flight(void)
{
	return __atomic_load_n(&tgm_idle.kicked, __ATOMIC_SEQ_CST) -
	       __atomic_load_n(&tgm_idle.retired, __ATOMIC_SEQ_CST);
}

static inline void idle_kicked(u32 first, u32 last)
{
	__atomic_fetch_add(&tgm_idle.kicked, (u64)(last - first) + 1, __ATOMIC_SEQ_CST);
}

/* The interrupt pass and tgm_chan_del can retire the same channel at once: the cursor moves by CAS, so no seqno is
 * counted twice (one double count would leave in-flight wrapped for the rest of the boot). */
static inline void idle_retire(struct tgm_chan *chan, u32 through)
{
	u32 seen = __atomic_load_n(&chan->gov_done, __ATOMIC_ACQUIRE);

	do {
		if ((s32)(through - seen) <= 0)
			return;
	} while (!__atomic_compare_exchange_n(&chan->gov_done, &seen, through, false,
					      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	__atomic_fetch_add(&tgm_idle.retired, (u64)(through - seen), __ATOMIC_SEQ_CST);
}

static inline void idle_fenced(struct tgm_chan *chan)
{
	idle_retire(chan, tgm_chan_completed(chan));
}
