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
/* The coherency registry's lock held: the entry deferred on the record (null: none), and taking it back */
void tgm_record_park(struct tgm_record *record, struct tgm_coh *c);
struct tgm_coh *tgm_record_take(struct tgm_record *record);
/* Whether IOSurface `id`, as the calling task finds it, is over the record's memory */
int tgm_record_is_surface(const struct tgm_record *record, uint32_t id);

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
