// SPDX-License-Identifier: MIT
// A CPU lock's view of a block-linear IOSurface (TegraGM20BViews.cpp). The GPU and the display read the surface's own pages
// tiled; a task that locks it sees its texels as rows: its mapping of the surface is replaced, at the same address, by one
// of a scratch the copy engine filled with the rows (behind the surface's GPU writers), and put back at its last unlock,
// after the copy engine wrote what the CPU wrote back into the texels. One scratch per surface serves every task that
// holds a lock on it. The mapping accessors are IOSurface's (TegraGM20BSurfaceClient.cpp, per IPSW through kits/SwitchABI).
#pragma once
#include <stdint.h>
#include "TegraGM20BTiled.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TegraGM20BSurfaceClient.cpp: the mapping of surface `id` in the task of IOSurfaceRootUserClient `client` (1; 0: none,
 * e.g. a surface over the client's own memory, or one made without CPU access; -1: one that is not of the whole
 * descriptor, which a view cannot put back), and the replacement of that range by `memory` (an IOMemoryDescriptor: a
 * scratch, or the surface's own to put it back). */
int tgm_cpu_view_of(void *client, uint32_t id, struct tgm_cpu_view *out);
int tgm_cpu_view_remap(const struct tgm_cpu_view *view, void *memory, uint64_t offset,
		       int prefault);
/* The descriptor the task's map of the surface is of, for putting a view back at view->offset (retained for the caller;
 * tgm_memory_release) */
void *tgm_surface_memory(void *client, uint32_t id);
void tgm_memory_release(void *memory);
#ifdef __cplusplus
class OSObject;
/* IOSurfaceRoot::lookupSurface(id, task): the surface, retained for the caller, or null */
OSObject *tgm_surface_lookup(uint32_t id, void *task);
OSObject *switch_gpu_surface_hold(uint32_t id, void *task, const uint64_t *pages, uint32_t count);
const volatile uint64_t *switch_gpu_surface_generation(OSObject *surface);
int switch_gpu_surface_client_memory(const OSObject *surface);
#endif
/* Whether surface `id` (looked up for `task`) lies in its creator's own memory, or cannot be looked up: such a surface
 * stays pitch */
int tgm_surface_client_memory(uint32_t id, void *task);
int switch_gpu_surface_pages_match(uint32_t id, void *task, const uint64_t *pages, uint32_t count);
struct SwitchGpuSurfaceGeometry;
struct switch_gpu_backing;
struct switch_gpu_cpu_lease;
int switch_gpu_surface_geometry(uint32_t id, void *task, const uint64_t *pages, uint32_t count,
				struct SwitchGpuSurfaceGeometry *geometry);
int switch_gpu_surface_backing(uint32_t id, void *task, struct switch_gpu_backing **backing);
int switch_gpu_surface_prepare(uint32_t id, void *task, uint32_t options,
			       struct switch_gpu_backing **backing,
			       struct switch_gpu_cpu_lease **owned, void **descriptor);
int switch_gpu_surface_prepare_cpu(void *surface, uint32_t id, void *task, uint32_t access,
				   struct switch_gpu_backing **backing,
				   struct switch_gpu_cpu_lease **owned, void **descriptor);
int switch_gpu_surface_prepare_gpu(void *surface, uint32_t id, void *task, uint32_t access,
				   struct switch_gpu_backing **backing,
				   struct switch_gpu_cpu_lease **owned, void **descriptor);
int switch_gpu_surface_gpu_admission(void *surface, struct switch_gpu_backing *expected,
				     uint32_t access);
void switch_gpu_surface_finish(void *descriptor);
int switch_gpu_surface_adopt(void *descriptor, struct switch_gpu_backing *backing);
void switch_gpu_surface_idle(void *descriptor);
uint32_t switch_gpu_surface_users(void *descriptor);
void switch_gpu_surface_descriptor_hold(void *descriptor, int hold);
int switch_gpu_surface_imported(void *surface, struct switch_gpu_backing *backing);
int switch_gpu_surface_layout_exclusive(void *surface, int enter);
/* Whether the user client behind a view still lives (its task may have died holding the lock) */
int tgm_cpu_view_live(const struct tgm_cpu_view *view);
void tgm_cpu_view_hold(struct tgm_cpu_view *view, int hold);

/* The scratch as a descriptor a task can map (the pool's), and its pages */
void *tgm_scratch_memory(struct tgm_scratch *scratch);
/* Gives a scratch a task may still map up for good: never handed out again */
void tgm_scratch_discard(struct tgm_scratch *scratch);

/* Lock (index 2) and unlock (3) of surface `id` through IOSurfaceRootUserClient `client`, before IOSurface's trap runs:
 * nonzero refuses the lock (kIOReturnCannotLock: kIOSurfaceLockAvoidSync and the GPU still writes it). Nothing for a
 * surface that is not block-linear. */
int tgm_views_trap(void *client, uint32_t index, uint32_t id, uint32_t options);
/* Before a GPU submission that writes the surface of `c` (any may be null): a write lock's rows kept to merge at its
 * unlock. One atomic load when no lock is held. Sleeps on the copy engine's fence interrupt while it copies. */
void tgm_views_gpu_writes(struct tgm_coh *c);
/* tgm_views_gpu_writes for each surface a job writes: every GPU submission path (Submit, SubmitJob) calls it once its
 * uses are resolved and before its job is kicked, since the kept rows are read behind the surface's earlier writers. */
void tgm_views_job_writes(struct tgm_coh *const *written, uint32_t count);

#ifdef __cplusplus
}
#endif
