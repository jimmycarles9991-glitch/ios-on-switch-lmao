/* SPDX-License-Identifier: MIT */
/*
 * GPU load from GM20B's PMU idle counters (gpu_load.c, debug kernels), the pair nvgpu programs for its raw load reading
 * (gk20a_pmu_init_perfmon_counter, gk20a/pmu_gk20a.c:754-830; nvgpu_pmu_get_load_counters, common/pmu/pmu_perfmon.c:
 * 282-305): counter 1 counts the cycles GR or CE2 (our copy engine) is busy, counter 2 every cycle, both unfiltered,
 * 31 bits each. Their clock is not named in nvgpu; the 5 s line prints counter 2's rate as measured.
 *
 * A read extends both to 64 bits. An interval is counted only when it cannot hide a wrap of counter 2: shorter than
 * 7/8 of 2^31 cycles at the fastest rate seen, or than TGM_LOAD_FIRST_BOUND_NS before a rate is seen. Anything else
 * (the first read, a power cycle, a longer gap) starts the counts over from that read. Plain data, so the host test
 * runs it.
 */
#ifndef TEGRA_GM20B_LOAD_H
#define TEGRA_GM20B_LOAD_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TGM_LOAD_MASK(i) (0x0010a504u + (i) * 16u)
#define TGM_LOAD_COUNT(i) (0x0010a508u + (i) * 16u)
#define TGM_LOAD_CTRL(i) (0x0010a50cu + (i) * 16u)

#define TGM_LOAD_BUSY 1u
#define TGM_LOAD_ALWAYS 2u

#define TGM_LOAD_GR_CE2 0x00200001u /* pwr_pmu_idle_mask_gr_enabled_f | _ce_2_enabled_f */
#define TGM_LOAD_COUNT_BITS 0x7fffffffu
#define TGM_LOAD_COUNT_RESET 0x80000000u
#define TGM_LOAD_CTRL_FIELDS 0x7u /* value [1:0], filter [2] */
#define TGM_LOAD_CTRL_BUSY 0x2u   /* value busy, filter disabled */
#define TGM_LOAD_CTRL_ALWAYS 0x3u /* value always, filter disabled */

#define TGM_LOAD_FIRST_BOUND_NS 2000000000ull /* 2^31 cycles of a clock up to 1 GHz */
#define TGM_LOAD_RATE_MIN_NS 10000000ull /* an interval this long or longer measures the rate */
#define TGM_LOAD_SAMPLE_NS 1000000000ull /* a submission reads when no read came for this long */

static inline uint32_t tgm_load_ctrl(uint32_t old, uint32_t value)
{
	return (old & ~TGM_LOAD_CTRL_FIELDS) | value;
}

/* Whether the counters still count as programmed. A power cycle (rail-gating, system sleep) loses the program; the
 * busy mask is written last, so it alone says the rest is in place. */
static inline int tgm_load_armed(uint32_t mask_busy)
{
	return mask_busy == TGM_LOAD_GR_CE2;
}

/* Everything since boot; a window is the difference of two copies. */
struct tgm_load {
	uint64_t busy, total; /* counted cycles */
	uint64_t counted_ns;  /* the intervals they were counted over */
	uint32_t reads,
		restarts; /* reads, and those that started the counts over after the first */
	uint64_t read_ns; /* the reads' own time */
	uint64_t
		rate_khz; /* counter 2's fastest rate over an interval of TGM_LOAD_RATE_MIN_NS or more */
	uint32_t raw_busy, raw_total; /* the counters at the last read */
	uint64_t last_ns;             /* the last read; 0: none */
};

/* Whether a submission at now_ns reads, the last read at last_ns (0: none yet). */
static inline int tgm_load_due(uint64_t last_ns, uint64_t now_ns)
{
	return last_ns == 0 || now_ns - last_ns >= TGM_LOAD_SAMPLE_NS;
}

static inline uint64_t tgm_load_bound_ns(uint64_t rate_khz)
{
	return rate_khz ? (uint64_t)TGM_LOAD_COUNT_BITS * 1000000ull / rate_khz * 7u / 8u
			: TGM_LOAD_FIRST_BOUND_NS;
}

/* A read at now_ns found the counters at busy and total; `fresh`: they were armed by this read, so start from here.
 * 1 when the interval since the last read was counted. */
static inline int tgm_load_step(struct tgm_load *l, uint64_t now_ns, uint32_t busy, uint32_t total,
				int fresh)
{
	const uint32_t db = (busy - l->raw_busy) & TGM_LOAD_COUNT_BITS;
	const uint32_t dt = (total - l->raw_total) & TGM_LOAD_COUNT_BITS;
	const uint64_t since = now_ns - l->last_ns;
	const int counted = !fresh && l->last_ns != 0 && now_ns > l->last_ns &&
			    since <= tgm_load_bound_ns(l->rate_khz) && db <= dt;

	if (counted) {
		l->busy += db;
		l->total += dt;
		l->counted_ns += since;
		if (since >= TGM_LOAD_RATE_MIN_NS &&
		    (uint64_t)dt * 1000000ull / since > l->rate_khz)
			l->rate_khz = (uint64_t)dt * 1000000ull / since;
	} else if (l->last_ns != 0) {
		l->restarts++;
	}
	l->raw_busy = busy & TGM_LOAD_COUNT_BITS;
	l->raw_total = total & TGM_LOAD_COUNT_BITS;
	l->last_ns = now_ns;
	l->reads++;
	return counted;
}

/* A window from two copies: busy share in tenths of a percent of the counted cycles, and the counted share of the
 * window's `ns`; -1 for a share with nothing to share. */
struct tgm_load_window {
	int busy_permille, counted_permille;
	uint32_t reads, restarts;
	uint64_t read_ns_each, rate_khz;
};

static inline struct tgm_load_window tgm_load_between(const struct tgm_load *was,
						      const struct tgm_load *now, uint64_t ns)
{
	struct tgm_load_window w;
	const uint64_t total = now->total - was->total;
	const uint64_t counted = now->counted_ns - was->counted_ns;

	w.busy_permille = total ? (int)((now->busy - was->busy) * 1000u / total) : -1;
	w.counted_permille = ns ? (int)((counted > ns ? ns : counted) * 1000u / ns) : -1;
	w.reads = now->reads - was->reads;
	w.restarts = now->restarts - was->restarts;
	w.read_ns_each = w.reads ? (now->read_ns - was->read_ns) / w.reads : 0;
	w.rate_khz = now->rate_khz;
	return w;
}

/*
 * Debug kernels (absent from a release kernel: weak-import it): the GPU's PMU cycles since the driver began counting,
 * `busy` (GR or the copy engine working) and `total` (every cycle), 64 bits each. A caller's share of an interval is
 * the difference of two calls' busy over their total's; equal totals mean the interval was not counted (the first call,
 * a power cycle, a gap too long to trust). 0, or -1 with nothing written while the GPU is powered down (it is idle
 * then) or before its bring-up finished. Thread context only, and never under a spin lock: entering the GPU's sleep
 * gate may take its mutex. Cost: three GPU register reads (the busy mask, which tells a power cycle, and the two
 * counters) under a spin lock; after a power cycle, five writes that program them again and a read back. A program
 * that does not hold turns the probe off with one line.
 */
int tegra_gm20b_gpu_load(uint64_t *busy, uint64_t *total);

/* gpu_stats_tick.c: each submission at now_ns (a read at most every TGM_LOAD_SAMPLE_NS), and the stats line's PMU
 * figure for its window of `window_ns` (one caller at a time). */
void tgm_load_sample(uint64_t now_ns);
void tgm_load_stats(char *buf, unsigned int len, uint64_t window_ns);

#ifdef __cplusplus
}
#endif

#endif /* TEGRA_GM20B_LOAD_H */
