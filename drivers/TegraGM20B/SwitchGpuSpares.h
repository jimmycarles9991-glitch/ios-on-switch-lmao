// SPDX-License-Identifier: MIT
// The size classes of cached BoAllocs and which ready spares serve each (TegraGM20BUserClientSpares.cpp spareTake), the
// classes kept ready from boot, why a BoAlloc found no zeroed spare and zeroed fresh pages on the allocating thread
// instead, and how deep a size class is kept warm after that.
#pragma once
#include <stdint.h>

enum switch_gpu_spare_miss : uint8_t {
	SWITCH_GPU_SPARE_FIRST,       // the class was not kept warm yet
	SWITCH_GPU_SPARE_ZEROING,     // its spares were still on the copy engine
	SWITCH_GPU_SPARE_DRAINED,     // warm, and every ready spare taken
	SWITCH_GPU_SPARE_PRESSURE,    // memory pressure: no spares kept
	SWITCH_GPU_SPARE_INELIGIBLE,  // contiguous: never served by a spare
	SWITCH_GPU_SPARE_RESERVED,    // its one ready spare is kept for the compositor
	SWITCH_GPU_SPARE_MISSES
};

// A spare's kind. Cached spares are cacheable pages (the cached BOs), write-combined ones a system coherent BO's
// buffer, whose own VM object holds the attribute (BoAlloc's PBENTRY note), so the two never serve each other. One
// budget under the cap holds both kinds, each class's depth learned from its own misses, with no share fixed between
// them; only the warm table's floors are fixed, all of them cached.
enum switch_gpu_spare_kind : uint32_t {
	SWITCH_GPU_SPARE_CACHED,
	SWITCH_GPU_SPARE_WC,
	SWITCH_GPU_SPARE_KINDS
};

constexpr uint64_t SWITCH_GPU_SPARE_UNIT_BYTES = 128ULL << 10;
constexpr uint64_t SWITCH_GPU_SPARE_UNITS_FROM = 512ULL << 10;
// 96 MiB plus the compositor's reserved 24 MiB spare (owner, 25 Nov: up to 24 MiB more wired for it)
constexpr uint64_t SWITCH_GPU_SPARE_CAP_BYTES = 120ULL << 20;

// The size classes cached BoAllocs and their spares are made in: 4 KiB to 64 KiB by doubling, then four to an octave, to
// 256 MiB (kMaxBoBytes)
constexpr uint32_t SWITCH_GPU_SPARE_CLASSES = 53;

constexpr uint64_t switch_gpu_spare_class_bytes(uint32_t c)
{
	return c < 5 ? 4096ULL << c : ((64ULL << 10) << ((c - 5) / 4)) * (5 + (c - 5) % 4) / 4;
}

// The smallest class of at least `bytes`; SWITCH_GPU_SPARE_CLASSES past the largest
constexpr uint32_t switch_gpu_spare_class_up(uint64_t bytes)
{
	uint32_t c = 0;
	while (c < SWITCH_GPU_SPARE_CLASSES && switch_gpu_spare_class_bytes(c) < bytes)
		c++;
	return c;
}

// Whether a ready spare of class `spare` serves a BoAlloc of class `asked`: its own class, or a larger one up to about
// twice its size (the BO is the spare's first pages)
constexpr bool switch_gpu_spare_serves(uint32_t asked, uint32_t spare)
{
	return spare >= asked && spare <= asked + (asked < 5 ? 1 : 4);
}

struct switch_gpu_spare_warm {
	uint64_t bytes;
	uint16_t ready;
	bool reserved;  // other clients leave the class's last ready spare to the compositor
};

// The classes kept ready from boot, before a miss teaches any a depth; a miss never takes one below its count here.
// 24 MiB: Photos' 1792x1472 RGBA16F attachment in backboardd (22 Nov: 17.3 ms of zero-fill and clean on its render
// thread, no spare of the class), which an app's launch target of the same size took first: two, the last one
// reserved for the compositor.
constexpr switch_gpu_spare_warm SWITCH_GPU_SPARE_WARM[] = {{64ULL << 10, 8, false},
							   {4ULL << 20, 2, false},
							   {8ULL << 20, 1, false},
							   {16ULL << 20, 2, false},
							   {24ULL << 20, 2, true}};

constexpr uint64_t switch_gpu_spare_warm_bytes()
{
	uint64_t bytes = 0;
	for (const switch_gpu_spare_warm &w : SWITCH_GPU_SPARE_WARM)
		bytes += w.bytes * w.ready;
	return bytes;
}

static_assert(switch_gpu_spare_warm_bytes() <= SWITCH_GPU_SPARE_CAP_BYTES,
	      "the refill makes no spare past the cap: a warm class above it is never ready");

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

// Whether a class's spares, ready or on their way (zeroing on the copy engine, or on the dirty list waiting for it), fall
// short of its depth. The refill makes one only then: a spare it made is counted from the moment it exists, so a copy
// engine too busy to take the dirty list bounds the refill at the depth, not at memory pressure.
constexpr bool switch_gpu_spare_short(uint16_t ready, uint16_t zeroing, uint16_t dirty,
				      uint16_t target)
{
	return uint32_t(ready) + zeroing + dirty < target;
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
// class of 16-32 MiB (a render server's dedicated chunk) learns a depth without crowding out the smaller ones; never
// below its warm count (`floor`), which the cap's static_assert already makes room for
inline uint16_t switch_gpu_spare_depth_cap(uint64_t bytes, uint64_t cap, uint16_t floor = 1)
{
	const uint64_t most = bytes ? cap / 4 / bytes : SWITCH_GPU_SPARE_DEPTH_MAX;
	const uint16_t least = floor ? floor : 1;
	return most < least                        ? least
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
