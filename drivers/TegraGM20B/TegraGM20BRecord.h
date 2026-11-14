// SPDX-License-Identifier: MIT
// The GPU's record of one IOSurface's memory (TegraGM20BRecord.cpp), kept in the kernel's GPU slot of the surface's
// IOMemoryDescriptor (SwitchMemoryAttachment) and released with it, so it lives exactly as long as the memory: whether a
// GPU import of the memory came before, and the block-linear coherency entry kept past its last import (deferred) until
// the memory goes, a CPU lock asks for rows, or an import takes it back.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tgm_coh;
struct tgm_record;

/* The record of `surface`'s memory, attached at the first call; null: none to be had. Not retained: it lives while the
 * caller holds the surface, and while an entry is deferred on it. */
struct tgm_record *tgm_record_of(void *surface);
/* 1 at the first call for the record (no GPU import of this memory came before), 0 after */
int tgm_record_first_import(struct tgm_record *record);
/* Whether a GPU import of the record's memory came */
int tgm_record_imported(struct tgm_record *record);
/* The coherency registry's lock held: the entry deferred on the record (null: none), and taking it back */
void tgm_record_park(struct tgm_record *record, struct tgm_coh *c);
struct tgm_coh *tgm_record_take(struct tgm_record *record);
/* Whether IOSurface `id`, as the calling task finds it, is over the record's memory */
int tgm_record_is_surface(const struct tgm_record *record, uint32_t id);

/* The processes that hold a CPU mapping of the record's memory, as IOSurface's create and lookup replies named them
 * (tgm_surface_mapped): the first one's ID, and whether another process mapped it since. Every IOSurfaceClient maps its
 * surface when it is made, so the first is the creator. Notes come from any process's create or lookup, with no thread
 * that owns the record and no order between them; one compare-and-swap per create or lookup (never per frame, job or
 * draw) is cheaper than a lock and sets the first exactly once. */
struct tgm_record_mappers {
	int32_t first;   /* atomic */
	uint32_t others; /* atomic */
};

struct tgm_record_mappers *tgm_record_mappers_of(struct tgm_record *record);

static inline void tgm_record_mappers_note(struct tgm_record_mappers *m, int32_t pid)
{
	int32_t first = 0;
	if (!__atomic_compare_exchange_n(&m->first, &first, pid, 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE) &&
	    first != pid)
		__atomic_store_n(&m->others, 1u, __ATOMIC_RELEASE);
}

/* Whether a process other than `pid` holds (or held) a CPU mapping of it */
static inline int tgm_record_mappers_other(const struct tgm_record_mappers *m, int32_t pid)
{
	const int32_t first = __atomic_load_n(&m->first, __ATOMIC_ACQUIRE);
	return (first != 0 && first != pid) || __atomic_load_n(&m->others, __ATOMIC_ACQUIRE) != 0;
}

/* TegraGM20BSurfaceClient.cpp: an IOSurfaceRootUserClient reply gave the calling process a CPU mapping of surface `id` */
void tgm_surface_mapped(uint32_t id, void *task);

/* TegraGM20BSurfaceClient.cpp: whether nothing has written IOSurface `surface` (an OSObject): its seed is still the
 * zero its shared entry was made with (no lock without kIOSurfaceLockReadOnly, no engine's device write end, not made
 * over its creator's memory), and it is neither a child of another surface nor made over ranges of a task's memory */
int tgm_surface_unwritten(void *surface);

/* TegraGM20BCoherency.cpp: the record's memory went; the entry deferred on it goes too, its texels never moved back */
void tgm_coherency_record_gone(struct tgm_record *record);
/* The same file: another engine is about to take the memory as rows; an entry deferred on the record has its texels
 * moved back first, unless the display or a kernel reader holds it block-linear */
void tgm_coherency_engine_rows(struct tgm_record *record);

#ifdef __cplusplus
}
#endif
