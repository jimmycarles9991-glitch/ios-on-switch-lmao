// SPDX-License-Identifier: MIT
// Which IOSurface creates TegraGM20BSurfacePrefault.cpp faults in: the kind's create methods of IOSurfaceRootUserClient
// and where their structure output holds the new surface's ID, then the surface's allocation size and the system's
// memory pressure. C++, no kernel headers: the host test runs these.
#pragma once
#include <stdint.h>
#include <string.h>

struct TegraGM20BSurfaceCreateAbi {
	uint32_t selectors[4]; /* the create methods; an unused entry is ~0u */
	uint32_t
		idOffset; /* the new surface's ID: a uint32_t at this byte of the create's structure output */
};

/* Smaller surfaces fault in where they are first touched: most are the CPU's (text, small images), which touches them at
 * once anyway, and a GPU import of one wires few pages. */
constexpr uint64_t kTegraGM20BPrefaultMinBytes = 256ULL << 10;

/* The new surface's ID when `selector` is a create whose output holds one; 0 for every other method. */
inline uint32_t tegra_gm20b_created_surface(const TegraGM20BSurfaceCreateAbi &abi,
					    uint32_t selector, const void *output,
					    uint32_t outputSize)
{
	bool create = false;
	for (const uint32_t s : abi.selectors)
		create = create || s == selector;
	if (!create || !output || abi.idOffset > outputSize ||
	    outputSize - abi.idOffset < sizeof(uint32_t))
		return 0;
	uint32_t id = 0;
	memcpy(&id, static_cast<const uint8_t *>(output) + abi.idOffset, sizeof(id));
	return id;
}

/* The surface an IOSurfaceRootUserClient reply of `replySize` bytes (IOSurface's reply fill, which every create and
 * lookup ends in: word 0 the client's CPU address, `idOffset` the surface's ID) gave the calling task a CPU mapping of;
 * 0 for any other output, and for a client that maps nothing (address 0). */
inline uint32_t tegra_gm20b_mapped_surface(uint32_t replySize, uint32_t idOffset,
					   const void *output, uint32_t outputSize)
{
	uint64_t address = 0;
	uint32_t id = 0;
	if (!output || !replySize || outputSize != replySize || idOffset > outputSize ||
	    outputSize - idOffset < sizeof(id) || outputSize < sizeof(address))
		return 0;
	memcpy(&address, output, sizeof(address));
	memcpy(&id, static_cast<const uint8_t *>(output) + idOffset, sizeof(id));
	return address ? id : 0;
}

/* Whether a created surface of `bytes` is faulted in now: large enough, and only at memorystatus' normal level, so the
 * early fault never raises compression or jetsam that the lazy fault would not. */
inline bool tegra_gm20b_prefault_wanted(uint64_t bytes, int pressureLevel)
{
	return pressureLevel == 0 && bytes >= kTegraGM20BPrefaultMinBytes;
}

/* A new surface's pages are zero-filled by the prefault through the cacheable physical aperture: it cleans them out to
 * DRAM, then keeps them wired until the surface's first GPU import (at most kTegraGM20BPrefaultHeld surfaces, the oldest
 * given back first, each for kTegraGM20BPrefaultHeldNs at most). */
constexpr uint32_t kTegraGM20BPrefaultHeld = 8;
constexpr uint64_t kTegraGM20BPrefaultHeldNs = 1000000000ULL;

template <typename Memory> struct TegraGM20BPrefaultHeld {
	struct Entry {
		Memory *memory;
		uint64_t deadline;
	};

	Entry entries[kTegraGM20BPrefaultHeld] = {};

	/* `memory` held until `deadline`; what that pushed out (the oldest), or null */
	Memory *hold(Memory *memory, uint64_t deadline)
	{
		Entry *slot = &entries[0];
		for (Entry &e : entries)
			if (!e.memory || (slot->memory && e.deadline < slot->deadline))
				slot = &e;
		Memory *out = slot->memory;
		*slot = {memory, deadline};
		return out;
	}

	Memory *take(const Memory *memory)
	{
		for (Entry &e : entries)
			if (e.memory && e.memory == memory) {
				Memory *held = e.memory;
				e = {};
				return held;
			}
		return nullptr;
	}

	/* One entry past its deadline at `now` (all of them under pressure), or null */
	Memory *expire(uint64_t now, bool pressure)
	{
		for (Entry &e : entries)
			if (e.memory && (pressure || e.deadline <= now)) {
				Memory *held = e.memory;
				e = {};
				return held;
			}
		return nullptr;
	}

	/* The earliest deadline held, 0 for none */
	uint64_t next() const
	{
		uint64_t first = 0;
		for (const Entry &e : entries)
			if (e.memory && (!first || e.deadline < first))
				first = e.deadline;
		return first;
	}
};

/* Whether no CPU wrote any of a held surface's `pages` since the prefault cleaned them: `modified(i)` reads the VM's
 * modified bit of page i, which the prefault cleared before its clean (pmap_clear_modify write-protects every mapping
 * of the page, so the next store through any task's mapping, IOSurfaceLock or not, faults and sets it again). */
template <typename Modified> inline bool tegra_gm20b_unwritten(uint32_t pages, Modified &&modified)
{
	for (uint32_t i = 0; i < pages; ++i)
		if (modified(i))
			return false;
	return true;
}

/* Whether a GPU import cleans the surface's pages on the CPU: not when another import's clean covers them, nor when it is
 * the surface's first, the GPU may write it, its seed is 0, and the prefault cleaned its pages after their zero-fill,
 * with them wired and unwritten (tegra_gm20b_unwritten) since. Those are every way a dirty line gets there: a zero-fill,
 * a fault after the pages were unwired, and a CPU store, locked or not. An import the GPU only reads still cleans: what
 * it samples is what a CPU put there. */
inline bool tegra_gm20b_import_cleans(bool covered, bool cleanedAtCreation, bool fresh,
				      bool gpuWrites)
{
	return !covered && !(cleanedAtCreation && fresh && gpuWrites);
}
