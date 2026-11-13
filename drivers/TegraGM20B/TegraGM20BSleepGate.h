/* SPDX-License-Identifier: MIT */
/*
 * System sleep's two pure pieces (gpu_power.c "system sleep"), kept here so the host test runs them (test/gm20b_sleep_test.cpp):
 *
 * The gate. Every path that touches the GPU without holding tgm_gpu.lock (a submission's kick, a new channel's first push, the
 * present, the pool scrubber) runs between tgm_gate_try_enter() and tgm_gate_leave(). Open, that costs two atomic
 * operations and no lock. tgm_gate_close() turns new callers back; once tgm_gate_drained() says nobody is inside, the GPU
 * may be powered down. The kernel adds the sleeping around it under one mutex: a caller turned back sleeps until the open
 * (or is refused), and the closer sleeps until a leave or a backed-out enter that returned "wake the closer". Those wake
 * under the same mutex the closer tests tgm_gate_drained() under, so the last leave is never missed.
 *
 * The plan: what sleep and wake do for the stage the bring-up reached.
 */
#ifndef TEGRA_GM20B_SLEEP_GATE_H
#define TEGRA_GM20B_SLEEP_GATE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tgm_gate {
	uint32_t shut;   /* 0 open; 1 from tgm_gate_close() to tgm_gate_open() */
	uint32_t inside; /* callers in, not yet out */
};

/* TGM_GATE_IN is counted (leave with tgm_gate_leave()); the others are not. TGM_GATE_SHUT_WAKE: this caller's back-out
 * let the gate drain, so it wakes the closer. */
enum tgm_gate_entry {
	TGM_GATE_IN = 0,
	TGM_GATE_SHUT = 1,
	TGM_GATE_SHUT_WAKE = 2,
};

/* The store of `shut` and the load of `inside` in tgm_gate_close()/tgm_gate_drained() pair with the add and the second
 * load here (all sequentially consistent): either the closer sees this caller inside, or this caller sees the gate shut. */
static inline enum tgm_gate_entry tgm_gate_try_enter(struct tgm_gate *g)
{
	if (__atomic_load_n(&g->shut, __ATOMIC_SEQ_CST))
		return TGM_GATE_SHUT;
	__atomic_add_fetch(&g->inside, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&g->shut, __ATOMIC_SEQ_CST))
		return TGM_GATE_IN;
	return __atomic_sub_fetch(&g->inside, 1, __ATOMIC_SEQ_CST) ? TGM_GATE_SHUT
								   : TGM_GATE_SHUT_WAKE;
}

/* 1: the gate is closing and this was the last caller inside: wake the closer. */
static inline int tgm_gate_leave(struct tgm_gate *g)
{
	return !__atomic_sub_fetch(&g->inside, 1, __ATOMIC_SEQ_CST) &&
	       __atomic_load_n(&g->shut, __ATOMIC_SEQ_CST);
}

static inline void tgm_gate_close(struct tgm_gate *g)
{
	__atomic_store_n(&g->shut, 1, __ATOMIC_SEQ_CST);
}

static inline int tgm_gate_drained(struct tgm_gate *g)
{
	return __atomic_load_n(&g->shut, __ATOMIC_SEQ_CST) &&
	       !__atomic_load_n(&g->inside, __ATOMIC_SEQ_CST);
}

static inline void tgm_gate_open(struct tgm_gate *g)
{
	__atomic_store_n(&g->shut, 0, __ATOMIC_SEQ_CST);
}

static inline int tgm_gate_is_shut(const struct tgm_gate *g)
{
	return __atomic_load_n(&g->shut, __ATOMIC_ACQUIRE) != 0;
}

/* NOTHING: no GPU was powered, nothing to do either way. POWER: powered, but no nvkm client (the bring-up stopped before
 * M2's end): power down and stay off. FULL: the client is up: quiesce, suspend nvkm, power down; at wake the reverse. */
enum tgm_sleep_plan {
	TGM_SLEEP_NOTHING = 0,
	TGM_SLEEP_POWER = 1,
	TGM_SLEEP_FULL = 2,
};

/* `stage`: the last bring-up stage that finished (gpu_bringup.c tgm_gpu.stage); `powered`: the GPU was powered (its nvkm device made). */
static inline enum tgm_sleep_plan tgm_sleep_plan(int stage, int powered)
{
	if (!powered)
		return TGM_SLEEP_NOTHING;
	return stage >= 2 ? TGM_SLEEP_FULL : TGM_SLEEP_POWER;
}

#ifdef __cplusplus
}
#endif
#endif
