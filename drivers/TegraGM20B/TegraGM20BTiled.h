// SPDX-License-Identifier: MIT
// Block-linear IOSurfaces (TegraGM20BTiled.cpp): each surface's layout, decided once on its coherency entry
// (TegraGM20BCoherency.cpp) and read by every GPU importer, the display and a CPU lock. The entry's side of it is a few
// accessors; the entry holds a `tgm_tiled` once decided and frees it with itself.
#pragma once
#include <stdint.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One task's mapping of a surface, as IOSurface made it for that task's IOSurfaceRootUserClient (IOSurfaceClient's
 * IOMemoryMap): the user client (retained while a lock through it is held), its task, address, length and options */
struct tgm_cpu_view {
	void *client, *task;
	uint64_t address, length, offset; /* offset: into the map's descriptor */
	uint32_t options;
};

#define TGM_VIEWS 8

struct tgm_tiled {
	int tiled; /* else pitch, for good */
	struct tgm_layout layout;
	uint32_t bytes_per_row;
	/* CPU locks (TegraGM20BViews.cpp; entry lock): the scratch holding the texels as rows while any is held, the locks,
	 * whether one of them may have written, whether a task still maps the scratch (its mapping could not be put back: the
	 * scratch is never reused), and the mappings pointed at the scratch with each one's locks */
	struct tgm_scratch *scratch;
	uint32_t locks;
	int wrote, stranded;

	struct {
		struct tgm_cpu_view view;
		uint32_t locks;
	} views[TGM_VIEWS];

	uint32_t nviews;
	uint64_t
		window_ns; /* debug: locks this second, and whether the surface was named for them */
	uint32_t window_locks;
	int named;
};

/* TegraGM20BCoherency.cpp: the entry's lock (held across a decision and a move), its layout slot (entry lock), whether
 * it is its surface's only import (refs 1), its page list (entry lock), and the GPU writers a move of it must follow
 * (still running; after a writer lost its slot, every channel's submitted work is waited for here and none is returned) */
void tgm_coh_lock(struct tgm_coh *c);
void tgm_coh_unlock(struct tgm_coh *c);
struct tgm_tiled **tgm_coh_tiled(struct tgm_coh *c);
int tgm_coh_alone(struct tgm_coh *c);
struct tgm_relayout_pages tgm_coh_pages(const struct tgm_coh *c);
uint32_t tgm_coh_writers(struct tgm_coh *c, struct tgm_relayout_writer *out, uint32_t max);
uint32_t tgm_coh_surface_id(const struct tgm_coh *c);
/* The block-linear entry of surface `id` with a reference (tgm_coherency_put drops it); null: none */
struct tgm_coh *tgm_coh_find_tiled(uint32_t id);
/* The same by a physical address inside the surface (any page of it): *offset its byte offset in the surface */
struct tgm_coh *tgm_coh_find_tiled_pa(uint64_t pa, uint64_t *offset);
/* The entry of the surface whose first page is at `pa` (tiled or not), with a reference for tgm_coherency_put, or null */
struct tgm_coh *tgm_coh_find_pa(uint64_t pa);
/* One more reference to an entry the caller holds one to */
void tgm_coh_hold(struct tgm_coh *c);

/* TegraGM20BRelayout.cpp: the scratch of rows a move goes through (kits/SwitchScratch's; null: none to be had), charged
 * to the locking task (`for_task` 1: the current task; 0: the kernel) */
struct tgm_scratch;
struct tgm_scratch *tgm_scratch_get(uint64_t bytes, int for_task, struct tgm_relayout_pages *pages);
void tgm_scratch_put(struct tgm_scratch *scratch);
void tgm_scratch_stats(uint64_t *held, uint64_t *peak);
void tgm_scratch_pages(const struct tgm_scratch *scratch, struct tgm_relayout_pages *pages);
void tgm_tiled_free(struct tgm_tiled *t);
/* Whether any surface is block-linear now: every trap and query returns at once while none is */
int tgm_tiled_any(void);
/* TegraGM20BViews.cpp: an import of `c` is going: locks held through user clients that died are dropped first, so a task
 * that died holding a lock keeps neither the scratch nor the surface's layout once its GPU imports are gone */
void tgm_views_sweep(struct tgm_coh *c);
/* Its last GPU import is going: a block-linear surface's texels go back to rows in place (behind its GPU writers) and it
 * is pitch from then on. Takes the entry lock. */
void tgm_tiled_untile(struct tgm_coh *c);

#ifdef __cplusplus
}
#endif
