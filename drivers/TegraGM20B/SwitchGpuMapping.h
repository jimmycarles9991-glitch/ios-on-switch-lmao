// SPDX-License-Identifier: MIT
#ifndef SWITCH_GPU_MAPPING_H
#define SWITCH_GPU_MAPPING_H
#include <stdint.h>

#define SWITCH_GPU_SMALL_SHIFT 12u
#define SWITCH_GPU_BIG_SHIFT 17u
#define SWITCH_GPU_BIG_BYTES (1ULL << SWITCH_GPU_BIG_SHIFT)
#define SWITCH_GPU_BIG_PAGES (1u << (SWITCH_GPU_BIG_SHIFT - SWITCH_GPU_SMALL_SHIFT))

struct switch_gpu_mapping_run {
	uint64_t bytes;
	uint8_t shift;
};

static inline int switch_gpu_mapping_big_page(const uint64_t *pages, uint32_t count, uint32_t first)
{
	if (!pages || first > count || SWITCH_GPU_BIG_PAGES > count - first ||
	    (pages[first] & (SWITCH_GPU_BIG_BYTES - 1)))
		return 0;
	for (uint32_t i = 1; i < SWITCH_GPU_BIG_PAGES; ++i)
		if (pages[first + i] != pages[first] + ((uint64_t)i << SWITCH_GPU_SMALL_SHIFT))
			return 0;
	return 1;
}

static inline int switch_gpu_mapping_next(const uint64_t *pages, uint32_t count, uint64_t va,
					  uint64_t offset, uint64_t bytes,
					  struct switch_gpu_mapping_run *run)
{
	const uint64_t size = (uint64_t)count << SWITCH_GPU_SMALL_SHIFT;
	if (!run || !pages || !bytes || ((va | offset | bytes) & 4095) || offset > size ||
	    bytes > size - offset || va > UINT64_MAX - bytes)
		return 0;
	const uint64_t mask = SWITCH_GPU_BIG_BYTES - 1;
	uint64_t at = 0;
	const int big = !(va & mask) && bytes >= SWITCH_GPU_BIG_BYTES &&
			switch_gpu_mapping_big_page(pages, count,
						    (uint32_t)(offset >> SWITCH_GPU_SMALL_SHIFT));
	while (at < bytes) {
		const int next_big =
			!((va + at) & mask) && bytes - at >= SWITCH_GPU_BIG_BYTES &&
			switch_gpu_mapping_big_page(
				pages, count, (uint32_t)((offset + at) >> SWITCH_GPU_SMALL_SHIFT));
		if (next_big != big)
			break;
		uint64_t step = SWITCH_GPU_BIG_BYTES - ((va + at) & mask);
		if (step > bytes - at)
			step = bytes - at;
		at += step;
	}
	run->bytes = at;
	run->shift = (uint8_t)(big ? SWITCH_GPU_BIG_SHIFT : SWITCH_GPU_SMALL_SHIFT);
	return 1;
}

/* The bytes of a range 128 KiB PTEs could map (VA and physical pages aligned and contiguous), whatever it is mapped with */
static inline uint64_t switch_gpu_mapping_big_bytes(const uint64_t *pages, uint32_t count,
						    uint64_t va, uint64_t offset, uint64_t bytes)
{
	struct switch_gpu_mapping_run run;
	uint64_t at = 0, big = 0;

	while (at < bytes &&
	       switch_gpu_mapping_next(pages, count, va + at, offset + at, bytes - at, &run)) {
		if (run.shift == SWITCH_GPU_BIG_SHIFT)
			big += run.bytes;
		at += run.bytes;
	}
	return big;
}
#endif
