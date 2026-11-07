/*
 * Copyright © 2026 Switch
 * SPDX-License-Identifier: MIT
 */

/* The host imports one submission reads and writes, as ExecSurfaces names
 * them to the kernel (TegraGM20BABI.h method 17): each BO once, written if any
 * use of it writes it. More than the record holds makes it
 * TegraGM20BExecSurfacesAll: the submission then waits on the GPU for nothing
 * and counts as a writer of every import, for CPU locks only.
 */
#ifndef NVKMD_DARWIN_SURFACES_H
#define NVKMD_DARWIN_SURFACES_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "TegraGM20BABI.h"

static inline void
darwin_surfaces_clear(struct TegraGM20BAbiExecSurfaces *s)
{
   s->count = 0;
   s->flags = 0;
   s->reserved = 0;
}

/* access: TegraGM20BSurfaceRead, TegraGM20BSurfaceWrite or both; none counts
 * as both
 */
static inline void
darwin_surfaces_add(struct TegraGM20BAbiExecSurfaces *s, uint64_t handle,
                    uint32_t access)
{
   access &= TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
   if (!access)
      access = TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
   for (uint32_t i = 0; i < s->count; i++) {
      if (s->uses[i].handle == handle) {
         s->uses[i].access |= access;
         return;
      }
   }
   if (s->count == TegraGM20BMaxExecSurfaces) {
      s->flags |= TegraGM20BExecSurfacesAll;
      return;
   }
   s->uses[s->count].handle = handle;
   s->uses[s->count].access = access;
   s->uses[s->count].reserved = 0;
   s->count++;
}

/* The record's length for the kernel: it ends after its uses */
static inline size_t
darwin_surfaces_bytes(const struct TegraGM20BAbiExecSurfaces *s)
{
   return offsetof(struct TegraGM20BAbiExecSurfaces, uses) +
          s->count * sizeof(s->uses[0]);
}

/* switch_nvk_submit_surfaces' hint: the imports the caller's next
 * vkQueueSubmit on `queue` touches. The call takes it from the caller's
 * thread before it makes any submission, and each submission it queues gets a
 * copy (Mesa's vk_queue_submit::driver_data), so the list goes wherever the
 * submission runs: on the caller's thread, or on Mesa's submit thread once the
 * queue has one.
 */
struct darwin_submit_hint {
   const void *queue;
   struct TegraGM20BAbiExecSurfaces list;
};

static inline void
darwin_hint_begin(struct darwin_submit_hint *hint, const void *queue)
{
   hint->queue = queue;
   darwin_surfaces_clear(&hint->list);
}

/* The thread's hint goes to the call on `queue` (taken), or is dropped: a
 * hint for another queue, or none
 */
static inline void
darwin_hint_take(struct darwin_submit_hint *hint,
                 struct darwin_submit_hint *taken, const void *queue)
{
   taken->queue = NULL;
   if (queue != NULL && hint->queue == queue)
      *taken = *hint;
   hint->queue = NULL;
}

/* One submission's copy of the call's list, freed with the submission; NULL:
 * the submission names none (also when out of memory: it then runs as one
 * without a list, which hides no write)
 */
static inline struct TegraGM20BAbiExecSurfaces *
darwin_hint_copy(const struct darwin_submit_hint *taken, const void *queue)
{
   if (queue == NULL || taken->queue != queue)
      return NULL;
   struct TegraGM20BAbiExecSurfaces *copy =
      (struct TegraGM20BAbiExecSurfaces *)malloc(sizeof(*copy));
   if (copy != NULL)
      *copy = taken->list;
   return copy;
}

#endif
