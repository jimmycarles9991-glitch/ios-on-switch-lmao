/* SPDX-License-Identifier: MIT */
/* The GPU clock's anchors. PTIMER restarts at every power-up of the GPU (nvkm_timer_init), so a PTIMER value names a time
 * only with the power-up it was taken after. The kernel takes one reading of PTIMER against the CPU's clock right after
 * each power-up's timer init, before any channel runs, and publishes it into every connection's fence page, which the
 * client and the GPU both read, so a sample converts with its own power-up's anchor with no call and no CPU wait.
 * PTIMER is reseeded past every value before the power-down (gpu_power.c stamp_reseed): each power-up's values lie at or
 * above its anchor and below the next one's, so a value's anchor is the newest one at or below it.
 * The stack key does not hash this header, so no unit it hashes (gpu/BUILD.bazel stack_key) may include it. */
#ifndef TEGRA_GM20B_CLOCK_ABI_H
#define TEGRA_GM20B_CLOCK_ABI_H
#include <stdint.h>
#include "TegraGM20BFencePageABI.h"

enum {
	TegraGM20BMethodClockAnchors =
		0x4341, /* 'CA': no input, reply TegraGM20BAbiClockAnchorsReply */
	TegraGM20BClockAnchorOffset =
		TegraGM20BFencePageClockAnchorsFirst, /* the table's byte offset in the connection's fence page */
	TegraGM20BClockAnchorSlots = 63,
};

/* PTIMER's units and mach_absolute_time in nanoseconds (absolutetime_to_nanoseconds) at the middle of the narrowest of
 * three windows around one read */
struct TegraGM20BAbiClockAnchor {
	uint64_t units, ns;
};

/* Anchor n (0 the boot's) is slot[n % TegraGM20BClockAnchorSlots]. The kernel writes slot[published % slots] and then
 * published + 1 (release): a reader reads published first (acquire) and takes the newest min(published, slots - 1),
 * never the slot the next power-up writes. */
struct TegraGM20BAbiClockAnchors {
	uint32_t published;
	uint32_t reserved[3];
	struct TegraGM20BAbiClockAnchor slot[TegraGM20BClockAnchorSlots];
};

struct TegraGM20BAbiClockAnchorsReply {
	uint64_t gpu_va; /* the table in the connection's GPU address space */
	uint32_t
		fence_token; /* the fence page's clientMemoryForType type: the table is at TegraGM20BClockAnchorOffset */
	uint32_t reserved;
};

/* The anchor of PTIMER value `units` in table `t` of `published` anchors into *out: 0 where no anchor kept is at or below
 * it */
static inline int tgm_clock_anchor_of(const struct TegraGM20BAbiClockAnchors *t, uint32_t published,
				      uint64_t units, struct TegraGM20BAbiClockAnchor *out)
{
	uint32_t kept =
		published < TegraGM20BClockAnchorSlots ? published : TegraGM20BClockAnchorSlots - 1;
	uint32_t i = 0;

	for (i = 0; i < kept; i++) {
		const struct TegraGM20BAbiClockAnchor *a =
			&t->slot[(published - 1 - i) % TegraGM20BClockAnchorSlots];
		if (a->units <= units) {
			*out = *a;
			return 1;
		}
	}
	return 0;
}

/* The anchor published after `published` others goes here */
static inline struct TegraGM20BAbiClockAnchor *
tgm_clock_anchor_slot(struct TegraGM20BAbiClockAnchors *t, uint32_t published)
{
	return &t->slot[published % TegraGM20BClockAnchorSlots];
}

/* The kernel's publish of anchor n of its own table `from` into a fence page's `to`: the slot, then the count */
static inline void tgm_clock_anchor_mirror(struct TegraGM20BAbiClockAnchors *to,
					   const struct TegraGM20BAbiClockAnchors *from, uint32_t n)
{
	*tgm_clock_anchor_slot(to, n) = from->slot[n % TegraGM20BClockAnchorSlots];
	__atomic_store_n(&to->published, n + 1, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiClockAnchor) == 16, "TegraGM20BAbiClockAnchor layout");
static_assert(sizeof(TegraGM20BAbiClockAnchors) == TegraGM20BFencePageClockAnchorsBytes,
	      "TegraGM20BAbiClockAnchors layout");
static_assert(sizeof(TegraGM20BAbiClockAnchorsReply) == 16,
	      "TegraGM20BAbiClockAnchorsReply layout");
#else
_Static_assert(sizeof(struct TegraGM20BAbiClockAnchor) == 16, "TegraGM20BAbiClockAnchor layout");
_Static_assert(sizeof(struct TegraGM20BAbiClockAnchors) == TegraGM20BFencePageClockAnchorsBytes,
	       "TegraGM20BAbiClockAnchors layout");
_Static_assert(sizeof(struct TegraGM20BAbiClockAnchorsReply) == 16,
	       "TegraGM20BAbiClockAnchorsReply layout");
#endif

#endif
