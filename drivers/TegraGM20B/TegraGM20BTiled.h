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

struct tgm_record;

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
	/* the texels as rows, read back before the first GPU submission that writes the surface under a write lock
	 * (tgm_views_gpu_writes): the last unlock merges; whether that was logged for the surface */
	struct tgm_scratch *base;
	int merged;
	/* the scratch the last read-only locks gave back, its handing out then, and the GPU writes it holds the rows of */
	struct tgm_scratch *kept;
	uint64_t kept_handout, kept_written, filled_written;

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
 * the caller's is its only reference and no CPU lock is held (alone: an untile's test), whether the same holds once the
 * askers' holds are left out (unshared: a decision's test; any other import, display hold or trap in flight still
 * counts), its page list (entry lock), and the GPU writers a move of it must follow (still running; after a writer lost
 * its slot, every channel's submitted work is waited for here and none is returned) */
void tgm_coh_lock(struct tgm_coh *c);
void tgm_coh_unlock(struct tgm_coh *c);
struct tgm_tiled **tgm_coh_tiled(struct tgm_coh *c);
int tgm_coh_alone(struct tgm_coh *c, uint32_t refs);
int tgm_coh_unshared(const struct tgm_coh *c);
int tgm_coh_pinned(const struct tgm_coh *c);
struct tgm_relayout_pages tgm_coh_pages(const struct tgm_coh *c);
uint32_t tgm_coh_writers(struct tgm_coh *c, struct tgm_relayout_writer *out, uint32_t max);
/* Until the GPU's writes to the pinned surface are in DRAM, which a CPU move reads and writes behind the L2 */
void tgm_coh_wait_visible(struct tgm_coh *c);
uint32_t tgm_coh_surface_id(const struct tgm_coh *c);
/* Every submission that may write the surface (tgm_coherency_wrote) and every write its backing records (an engine's
 * job, a CPU writer's release): unchanged, nothing wrote it since */
uint64_t tgm_coh_written(const struct tgm_coh *c);
/* The block-linear entry of surface `id` with a reference (tgm_coherency_put drops it); null: none */
struct tgm_coh *tgm_coh_find_tiled(uint32_t id);
/* The same by a physical address inside the surface (any page of it): *offset its byte offset in the surface */
struct tgm_coh *tgm_coh_find_tiled_pa(uint64_t pa, uint64_t *offset);
/* The entry of the surface whose first page is at `pa` (tiled or not), with a reference for tgm_coherency_put, or null */
struct tgm_coh *tgm_coh_find_pa(uint64_t pa);
/* One more reference to an entry the caller holds one to */
void tgm_coh_hold(struct tgm_coh *c);
/* The same, for the importer asking the surface's layout (tgm_coherency_layout) with its connection's lock dropped: it
 * keeps the entry alive, and the decision does not count it as another user. tgm_coh_put_asker drops it. */
void tgm_coh_hold_asker(struct tgm_coh *c);
void tgm_coh_put_asker(struct tgm_coh *c);

/* TegraGM20BRelayout.cpp: the scratch of rows a move goes through (kits/SwitchScratch's; null: none to be had), charged
 * to the locking task (`for_task` 1: the current task; 0: the kernel) */
struct tgm_scratch;
struct tgm_scratch *tgm_scratch_get(uint64_t bytes, int for_task, struct tgm_relayout_pages *pages);
void tgm_scratch_put(struct tgm_scratch *scratch);
/* The scratch put back after handing out `handout` again, if no one took it since: *intact, its pages kept their bytes */
struct tgm_scratch *tgm_scratch_reclaim(struct tgm_scratch *scratch, uint64_t handout, int *intact);
uint64_t tgm_scratch_handout(const struct tgm_scratch *scratch);
void tgm_scratch_stats(uint64_t *held, uint64_t *peak);
void tgm_scratch_pages(const struct tgm_scratch *scratch, struct tgm_relayout_pages *pages);
/* The scratch's bytes, cacheable, in the kernel's address space until tgm_scratch_unmap(*map); null when it cannot be
 * mapped */
void *tgm_scratch_map(struct tgm_scratch *scratch, void **map);
void tgm_scratch_unmap(void *map);
void tgm_tiled_free(struct tgm_tiled *t);
/* Whether any surface is block-linear now: every trap and query returns at once while none is */
int tgm_tiled_any(void);
/* TegraGM20BViews.cpp: an import of `c` is going: locks held through user clients that died are dropped first, so a task
 * that died holding a lock keeps neither the scratch nor the surface's layout once its GPU imports are gone */
void tgm_views_sweep(struct tgm_coh *c);
/* Its last GPU import is going: a block-linear surface's texels go back to rows in place (behind its GPU writers) and it
 * is pitch from then on. Takes the entry lock. */
int tgm_tiled_untile(struct tgm_coh *c);
/* The same for a deferred entry a CPU lock found (its reference and the record's are the only ones; nonzero: not moved,
 * still block-linear) */
int tgm_tiled_untile_deferred(struct tgm_coh *c);

/* TegraGM20BCoherency.cpp: an import of surface memory noted on its entry (BoImport, after the pin): the memory's record,
 * and whether nothing wrote it before this, the first GPU import of it (the entry's first decision may then skip the
 * tiling move: no rows to move) */
void tgm_coherency_import(struct tgm_coh *c, struct tgm_record *record, int fresh);
/* Entry lock held: whether the first decision moves nothing (tgm_coherency_import's fresh, still no write: no submission,
 * no backing write, the surface's seed unmoved) */
int tgm_coh_fresh(const struct tgm_coh *c);
/* Whether the entry is deferred on its record: 1 and the record is surface `id`'s as the calling task finds it, -1 and it
 * is another's (a surface that is going, whose ID was taken again), 0 not deferred */
int tgm_coh_deferred_for(struct tgm_coh *c, uint32_t id);
/* Takes a deferred entry back from its record and drops the reference the record held */
void tgm_coh_undefer(struct tgm_coh *c);
/* A GPU import of the memory whose first page is `pa` is starting: an entry deferred there has its texels moved back to
 * rows first (an importer that never asks binds rows), unless the display or a kernel reader holds it block-linear */
void tgm_coherency_rows_at(uint64_t pa);
/* Whether any entry is deferred now (no lock taken) */
int tgm_coh_any_deferred(void);
/* TegraGM20BViews.cpp: a CPU lock of surface `id` starts, before its lease (SwitchGpuSurfaceAccess.cpp): a deferred
 * surface is untiled first, so the lease and the lock see rows */
void tgm_views_deferred_lock(uint32_t id, uint32_t options);

/* TegraGM20BTiled.cpp's relayout counts (debug kernels' stats line; the calling process's GPU path counts) */
enum tgm_relayout_count {
	TGM_RELAYOUT_FRESH, /* a first decision's tiling move skipped: nothing had written the surface */
	TGM_RELAYOUT_DEFERRED, /* a last import's untile move left for a CPU lock */
	TGM_RELAYOUT_LAZY,     /* a deferred untile a CPU lock made */
	TGM_RELAYOUT_NEVER_UNTILED, /* a deferred surface's memory went block-linear: its untile never ran */
	TGM_RELAYOUT_MOVED, /* a tiling or untiling move made */
	TGM_RELAYOUT_COUNTS
};

void tgm_tiled_count(enum tgm_relayout_count which);

#ifdef __cplusplus
}
#endif
