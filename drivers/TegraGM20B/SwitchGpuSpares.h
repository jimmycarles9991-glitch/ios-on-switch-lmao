// SPDX-License-Identifier: MIT
// Why a cached BoAlloc found no zeroed spare (TegraGM20BUserClientPool.cpp spareTake) and zeroed fresh pages on the
// allocating thread instead, and how deep a size class is kept warm after that.
#pragma once
#include <stdint.h>

enum switch_gpu_spare_miss : uint8_t {
	SWITCH_GPU_SPARE_FIRST,       // the class was not kept warm yet
	SWITCH_GPU_SPARE_ZEROING,     // its spares were still on the copy engine
	SWITCH_GPU_SPARE_DRAINED,     // warm, and every ready spare taken
	SWITCH_GPU_SPARE_PRESSURE,    // memory pressure: no spares kept
	SWITCH_GPU_SPARE_INELIGIBLE,  // contiguous or write-combined: never served by a spare
	SWITCH_GPU_SPARE_MISSES
};

inline switch_gpu_spare_miss switch_gpu_spare_miss_of(uint16_t target, uint16_t zeroing,
						      bool pressure)
{
	if (pressure)
		return SWITCH_GPU_SPARE_PRESSURE;
	if (!target)
		return SWITCH_GPU_SPARE_FIRST;
	return zeroing ? SWITCH_GPU_SPARE_ZEROING : SWITCH_GPU_SPARE_DRAINED;
}

constexpr uint16_t SWITCH_GPU_SPARE_DEPTH_MAX = 8;

// A class learns a depth of 1 at its first miss and doubles it at each later one, to SWITCH_GPU_SPARE_DEPTH_MAX: a burst
// of N allocations of one class at depth 1 zeroes N - 1 of them on the allocating thread. The pool's byte cap still
// bounds what the refill makes.
inline uint16_t switch_gpu_spare_depth_after_miss(uint16_t target, bool learnable,
						  switch_gpu_spare_miss why)
{
	if (why == SWITCH_GPU_SPARE_PRESSURE || why == SWITCH_GPU_SPARE_INELIGIBLE || !learnable)
		return target;
	if (!target)
		return 1;
	const uint16_t doubled = static_cast<uint16_t>(target * 2u);
	return target >= SWITCH_GPU_SPARE_DEPTH_MAX
		       ? target
		       : (doubled < SWITCH_GPU_SPARE_DEPTH_MAX ? doubled
							       : SWITCH_GPU_SPARE_DEPTH_MAX);
}
