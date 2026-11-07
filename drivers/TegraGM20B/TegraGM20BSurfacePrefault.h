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
	uint32_t id;
	memcpy(&id, static_cast<const uint8_t *>(output) + abi.idOffset, sizeof(id));
	return id;
}

/* Whether a created surface of `bytes` is faulted in now: large enough, and only at memorystatus' normal level, so the
 * early fault never raises compression or jetsam that the lazy fault would not. */
inline bool tegra_gm20b_prefault_wanted(uint64_t bytes, int pressureLevel)
{
	return pressureLevel == 0 && bytes >= kTegraGM20BPrefaultMinBytes;
}
