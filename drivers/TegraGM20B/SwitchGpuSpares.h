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

constexpr uint64_t SWITCH_GPU_SPARE_UNIT_BYTES = 128ULL << 10;
constexpr uint64_t SWITCH_GPU_SPARE_UNITS_FROM = 512ULL << 10;

struct switch_gpu_spare_units {
	uint32_t units;
	uint64_t tail;
};

// A spare of `bytes` made by the refill thread: physically contiguous, 128 KiB-aligned units (one GMMU big page each),
// then the rest in pages. A class below SWITCH_GPU_SPARE_UNITS_FROM would gain at most three big pages, and its BOs are
// the ones mapped with kIOMapPrefault, which wants one buffer.
inline switch_gpu_spare_units switch_gpu_spare_layout(uint64_t bytes)
{
	if (bytes < SWITCH_GPU_SPARE_UNITS_FROM)
		return {0, bytes};
	return {uint32_t(bytes / SWITCH_GPU_SPARE_UNIT_BYTES), bytes % SWITCH_GPU_SPARE_UNIT_BYTES};
}

inline switch_gpu_spare_miss switch_gpu_spare_miss_of(uint16_t target, uint16_t zeroing,
						      bool pressure)
{
	if (pressure)
		return SWITCH_GPU_SPARE_PRESSURE;
	if (!target)
		return SWITCH_GPU_SPARE_FIRST;
	return zeroing ? SWITCH_GPU_SPARE_ZEROING : SWITCH_GPU_SPARE_DRAINED;
}

// Whether the spares go back to the VM: a VM pressure level above normal, or available pages within `cap_pages` of the
// long-idle reaper's threshold (0: no reaper). xnu-12377's memorystatus kills every process idle past its minimum age
// once available pages fall below that threshold (five times the critical level), while the pressure level still reads
// normal: zeroed spares kept wired then cost idle daemons and extensions their lives.
constexpr bool switch_gpu_spares_give_back(int pressure_level, uint64_t available_pages,
					   uint64_t reaper_threshold_pages, uint64_t cap_pages)
{
	return pressure_level != 0 || (reaper_threshold_pages != 0 &&
				       available_pages < reaper_threshold_pages + cap_pages);
}

constexpr uint16_t SWITCH_GPU_SPARE_DEPTH_MAX = 8;

// The deepest a class of `bytes` is kept: its spares together a quarter of the pool's cap at most, one at least, so a
// class of 16-32 MiB (a render server's dedicated chunk) learns a depth without crowding out the smaller ones
inline uint16_t switch_gpu_spare_depth_cap(uint64_t bytes, uint64_t cap)
{
	const uint64_t most = bytes ? cap / 4 / bytes : SWITCH_GPU_SPARE_DEPTH_MAX;
	return most < 1                            ? 1
	       : most < SWITCH_GPU_SPARE_DEPTH_MAX ? uint16_t(most)
						   : SWITCH_GPU_SPARE_DEPTH_MAX;
}

// The class to give a ready spare back from so that a class below its depth fits under the cap: the largest class
// (classes by size, smallest first) holding more ready spares than its depth, or `classes` when none does. Freed BOs are
// kept while they fit, so without this a pool full of classes no one asks for refills none that someone does.
// Whether a class of `need` bytes fits under `cap` once the spares kept past their depth (`over` bytes of the `clean`)
// are given back: when it does not, none are given back for it
inline bool switch_gpu_spare_fits_after(uint64_t clean, uint64_t over, uint64_t need, uint64_t cap)
{
	return clean - over + need <= cap;
}

inline uint32_t switch_gpu_spare_over_depth(const uint16_t *count, const uint16_t *target,
					    uint32_t classes)
{
	for (uint32_t c = classes; c-- > 0;)
		if (count[c] > target[c])
			return c;
	return classes;
}

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
