/* SPDX-License-Identifier: MIT */
/* What an import's layout ask (SurfaceLayout, method 16 and 24) passes the surface's decision (TegraGM20BTiled.cpp). A
 * surface may tile when the decision is the asker's alone (no other import mid-ask), its texels are packed, it does not
 * lie over its creator's memory (read there without IOSurfaceLock) and the asked shape is the surface's. Then it tiles
 * when the asker writes it, or when it only samples a surface nothing has written yet (seed 0): no CPU producer has
 * shown itself, so no lock pays a copy-engine move on its account. */
#pragma once

#define TGM_LAYOUT_ASK_WRITES 1
#define TGM_LAYOUT_ASK_FIRST 2 /* the BO's first ask (a failed tiling move is decided again then) */
#define TGM_LAYOUT_ASK_UNWRITTEN 4

static inline int tgm_layout_ask_flags(int exclusive, int packed, int client_memory, int shape_ok,
				       int asks_writes, int unwritten, int first)
{
	const int tileable = exclusive && packed && !client_memory && shape_ok;

	return (tileable && asks_writes ? TGM_LAYOUT_ASK_WRITES : 0) |
	       (tileable && !asks_writes && unwritten ? TGM_LAYOUT_ASK_UNWRITTEN : 0) |
	       (first ? TGM_LAYOUT_ASK_FIRST : 0);
}
