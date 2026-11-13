/* SPDX-License-Identifier: MIT */
/* The debug lines' cost histograms (gpu_stats.c's imports, deadline.c's compositor frames): quarter-octave buckets of
 * microseconds (each about 19% wide; below 4 us one per microsecond), so a percentile reads as the upper bound of the
 * bucket it falls in. */
#ifndef TEGRA_GM20B_COST_HIST_H
#define TEGRA_GM20B_COST_HIST_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TGM_COST_BUCKETS 100

static inline uint32_t tgm_cost_bucket(uint64_t ns)
{
	const uint64_t us = ns / 1000;
	uint32_t octave = 0;
	uint32_t b = 0;

	if (us < 4)
		return (uint32_t)us;
	octave = 63u - (uint32_t)__builtin_clzll(us);
	b = octave * 4 + ((uint32_t)(us >> (octave - 2)) & 3);
	return b < TGM_COST_BUCKETS - 1 ? b : TGM_COST_BUCKETS - 1;
}

static inline uint64_t tgm_cost_bucket_top_us(uint32_t b)
{
	return b < 4 ? b : ((uint64_t)(4 + (b & 3) + 1) << (b / 4 - 2)) - 1;
}

/* The upper bound (us) of the bucket holding the `per_mille`th part of the window's `n` samples, taken from `hist`. */
static inline uint64_t tgm_cost_percentile(const uint32_t *hist, uint64_t n, uint32_t per_mille)
{
	const uint64_t want = (n * per_mille + 999) / 1000;
	uint64_t seen = 0;
	uint32_t b = 0;

	for (b = 0; b < TGM_COST_BUCKETS; b++) {
		seen += hist[b];
		if (seen >= want && seen)
			return tgm_cost_bucket_top_us(b);
	}
	return 0;
}

#ifdef __cplusplus
}
#endif
#endif
