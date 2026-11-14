/* SPDX-License-Identifier: MIT */
/*
 * System sleep's two pure pieces (gpu_power.c "system sleep"), kept here so the host test runs them (test/gm20b_sleep_test.cpp):
 *
 * The gate. Every path that touches the GPU without holding tgm_gpu.lock (a submission's kick, a new channel's first push, the
 * present, the pool scrubber) runs between tgm_gate_try_enter() and tgm_gate_leave(). Open, either is a store to the
 * caller's own CPU's counts and one fence: no lock, and no line another CPU writes. tgm_gate_close() turns new callers
 * back; once tgm_gate_drained() says nobody is inside, the GPU may be powered down. The kernel adds the sleeping around it
 * under one mutex: a caller turned back sleeps until the open (or is refused), and the closer sleeps until a leave or a
 * backed-out enter that returned "wake the closer". Those wake under the same mutex the closer tests tgm_gate_drained()
 * under, so the last leave is never missed.
 *
 * The plan: what sleep and wake do for the stage the bring-up reached.
 */
#ifndef TEGRA_GM20B_SLEEP_GATE_H
#define TEGRA_GM20B_SLEEP_GATE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Both kernels' MAX_CPUS (pexpert's BCM2837 board config): every cpu_number() is below it. */
#define TGM_GATE_CPUS 8

/* A caller may enter on one CPU and leave on another: only the sums over every cell mean anything. */
struct tgm_gate_cell {
	uint64_t enters;
	uint64_t leaves;
} __attribute__((aligned(64)));

struct tgm_gate {
	uint32_t shut; /* 0 open; 1 from tgm_gate_close() to tgm_gate_open() */
	struct tgm_gate_cell cells[TGM_GATE_CPUS];
};

/* TGM_GATE_IN is counted (leave with tgm_gate_leave()); the others are not. TGM_GATE_SHUT_WAKE: this caller's back-out
 * let the gate drain, so it wakes the closer. */
enum tgm_gate_entry {
	TGM_GATE_IN = 0,
	TGM_GATE_SHUT = 1,
	TGM_GATE_SHUT_WAKE = 2,
};

/* Each leave is read before the enters, and acquires the enter its caller made first (on whichever CPU), so the sum
 * never counts a leave without its enter. */
static inline uint64_t tgm_gate_inside(const struct tgm_gate *g)
{
	uint64_t enters = 0;
	uint64_t leaves = 0;
	unsigned int cpu = 0;

	for (cpu = 0; cpu < TGM_GATE_CPUS; cpu++)
		leaves += __atomic_load_n(&g->cells[cpu].leaves, __ATOMIC_ACQUIRE);
	for (cpu = 0; cpu < TGM_GATE_CPUS; cpu++)
		enters += __atomic_load_n(&g->cells[cpu].enters, __ATOMIC_RELAXED);
	return enters - leaves;
}

/* `cpu`'s counts have one writer at a time: the caller stays on that CPU, with nothing else running there, until the
 * enter or leave returns (the kernel: preemption off, and only threads use the gate). The release orders what the
 * caller did before it ahead of the count. */
static inline void tgm_gate_count(uint64_t *count)
{
	__atomic_store_n(count, __atomic_load_n(count, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
}

/* The fence after this caller's count and the one tgm_gate_close() puts after `shut` pair: either the closer sees this
 * caller inside, or this caller sees the gate shut. Of callers leaving or backing out together, the one whose fence comes
 * last sees every other's leave, so the last out always finds the gate empty. */
static inline enum tgm_gate_entry tgm_gate_try_enter(struct tgm_gate *g, unsigned int cpu)
{
	struct tgm_gate_cell *cell = &g->cells[cpu];

	if (__atomic_load_n(&g->shut, __ATOMIC_RELAXED))
		return TGM_GATE_SHUT;
	tgm_gate_count(&cell->enters);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&g->shut, __ATOMIC_RELAXED))
		return TGM_GATE_IN;
	tgm_gate_count(&cell->leaves);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return tgm_gate_inside(g) ? TGM_GATE_SHUT : TGM_GATE_SHUT_WAKE;
}

/* 1: the gate is closing and this was the last caller inside: wake the closer. */
static inline int tgm_gate_leave(struct tgm_gate *g, unsigned int cpu)
{
	tgm_gate_count(&g->cells[cpu].leaves);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return __atomic_load_n(&g->shut, __ATOMIC_RELAXED) && !tgm_gate_inside(g);
}

static inline void tgm_gate_close(struct tgm_gate *g)
{
	__atomic_store_n(&g->shut, 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static inline int tgm_gate_drained(const struct tgm_gate *g)
{
	return __atomic_load_n(&g->shut, __ATOMIC_ACQUIRE) && !tgm_gate_inside(g);
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
