/*
 * Copyright © 2026 Switch
 * SPDX-License-Identifier: MIT
 */

/* Channels the kernel killed for a GPU fault or hang (TegraGM20BABI.h method
 * 11, TegraGM20BAbiFence): which seqnos never ran to their end, and the
 * timeline values whose signals were among them. Metal fails the command
 * buffer a fault strikes, and those committed while it ran, and runs the
 * queue on; the client asks for a value's failure once the value is reached
 * (switch_nvk_semaphore_failure), so a timeline keeps its failed values from
 * the moment it folds them in.
 */
#ifndef NVKMD_DARWIN_KILL_H
#define NVKMD_DARWIN_KILL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "TegraGM20BABI.h"

struct darwin_kill {
   uint32_t channel; /* 0: none */
   uint32_t first, last;
   uint32_t error;
};

static inline bool
darwin_kill_covers(const struct darwin_kill *k, uint32_t channel,
                   uint32_t seqno)
{
   return k->channel != 0 && k->channel == channel &&
          (int32_t)(seqno - k->first) >= 0 && (int32_t)(k->last - seqno) >= 0;
}

/* The kill record of `channel`'s fence word; false while the channel lives.
 * The first barrier orders it after the caller's read of `completed`.
 */
static inline bool
darwin_kill_read(const volatile struct TegraGM20BAbiFence *fence,
                 uint32_t channel, struct darwin_kill *out)
{
   __atomic_thread_fence(__ATOMIC_ACQUIRE);
   const uint32_t error = fence->error;
   if (error == 0)
      return false;
   __atomic_thread_fence(__ATOMIC_ACQUIRE);
   out->channel = channel;
   out->first = fence->killed_first;
   out->last = fence->killed_last;
   out->error = error;
   return true;
}

/* A device's channels killed and made again: a slot's seqnos continue in the
 * channel that replaces it, so a record outlives its channel for the syncs
 * still pending on it. The oldest goes first.
 */
#define DARWIN_KILLS 8

struct darwin_kills {
   struct darwin_kill kill[DARWIN_KILLS];
   uint32_t next;
};

static inline void
darwin_kills_add(struct darwin_kills *k, const struct darwin_kill *kill)
{
   k->kill[k->next++ % DARWIN_KILLS] = *kill;
}

static inline uint32_t
darwin_kills_error(const struct darwin_kills *k, uint32_t channel,
                   uint32_t seqno)
{
   for (uint32_t i = 0; i < DARWIN_KILLS; i++) {
      if (darwin_kill_covers(&k->kill[i], channel, seqno))
         return k->kill[i].error;
   }
   return 0;
}

/* A timeline's failed values, in ranges of consecutive values (one kill fails
 * every submission its channel had in flight). Made at the first failure.
 */
#define DARWIN_FAILURES 16

struct darwin_failures {
   struct {
      uint64_t first, last;
      uint32_t error;
   } range[DARWIN_FAILURES];
   uint32_t count, next;
};

static inline void
darwin_failures_add(struct darwin_failures **pf, uint64_t value, uint32_t error)
{
   if (*pf == NULL) {
      *pf = (struct darwin_failures *)calloc(1, sizeof(**pf));
      if (*pf == NULL)
         return;
   }
   struct darwin_failures *f = *pf;
   if (f->count != 0) {
      const uint32_t newest = (f->next + DARWIN_FAILURES - 1) % DARWIN_FAILURES;
      if (f->range[newest].error == error &&
          value >= f->range[newest].first &&
          value <= f->range[newest].last + 1) {
         if (value > f->range[newest].last)
            f->range[newest].last = value;
         return;
      }
   }
   f->range[f->next].first = value;
   f->range[f->next].last = value;
   f->range[f->next].error = error;
   f->next = (f->next + 1) % DARWIN_FAILURES;
   if (f->count < DARWIN_FAILURES)
      f->count++;
}

static inline uint32_t
darwin_failures_find(const struct darwin_failures *f, uint64_t value)
{
   if (f == NULL)
      return 0;
   for (uint32_t i = 0; i < f->count; i++) {
      if (value >= f->range[i].first && value <= f->range[i].last)
         return f->range[i].error;
   }
   return 0;
}

#endif /* NVKMD_DARWIN_KILL_H */
