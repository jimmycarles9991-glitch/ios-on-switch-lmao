/*
 * Copyright © 2026 Switch
 * SPDX-License-Identifier: MIT
 */

/* WAIT_ANY over nvkmd_darwin's syncs, sleeping until one resolves.
 *
 * A host signal or a submission broadcasts the process's sync condition; a
 * GPU fence passing is the kernel's fence interrupt, which only WaitFence
 * sleeps on, one channel and seqno at a time. So every pending fence of the
 * set gets a watcher thread asleep in WaitFence, which broadcasts the
 * condition when it returns, and the caller sleeps on the condition until a
 * sync of the set resolves or the deadline passes. A watcher outlives a wait
 * that returned first: it holds a copy of the connection, never the device.
 *
 * The includer defines DARWIN_ANY_CONN, the connection type a watcher's
 * WaitFence goes through.
 */
#ifndef NVKMD_DARWIN_WAIT_ANY_H
#define NVKMD_DARWIN_WAIT_ANY_H

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef DARWIN_ANY_CONN
#error "DARWIN_ANY_CONN: the connection type a watcher's WaitFence goes through"
#endif

/* A fence's wake that never came: the deadline passed with the fence found
 * complete and its watcher still asleep this long after.
 */
#define DARWIN_ANY_WAKE_GRACE_NS 50000000ull

enum darwin_any_status {
   DARWIN_ANY_WAITING, /* for a host signal or a submission */
   DARWIN_ANY_FENCE,   /* for a submitted GPU fence */
   DARWIN_ANY_DONE,
};

struct darwin_any_fence {
   DARWIN_ANY_CONN conn;
   void *dev; /* for fence_done, while the wait lasts */
   uint32_t channel, seqno;
};

struct darwin_any_ops {
   pthread_mutex_t *lock;
   pthread_cond_t *cond;
   /* lock held: where wait i stands (*fence, for DARWIN_ANY_FENCE) */
   enum darwin_any_status (*status)(void *ctx, uint32_t i,
                                    struct darwin_any_fence *fence);
   /* lock held */
   bool (*fence_done)(const struct darwin_any_fence *fence);
   /* any thread, no lock: sleeps in the kernel until the fence passes or
    * timeout_ns (UINT64_MAX: none); 0, ETIMEDOUT, or another errno
    */
   int (*wait_fence)(const struct darwin_any_fence *fence, uint64_t timeout_ns);
   /* lock held: sleeps until a broadcast or abs_ns (monotonic); 0 or ETIMEDOUT */
   int (*cond_wait)(uint64_t abs_ns);
   uint64_t (*now_ns)(void);
   /* lock held: the wake of a passed fence never came (a lost interrupt) */
   void (*lost)(const struct darwin_any_fence *fence);
};

struct darwin_any_watcher {
   struct darwin_any_fence fence;
   uint64_t timeout_ns;
   const struct darwin_any_ops *ops;
   unsigned refs; /* lock */
   bool fired;    /* lock: its WaitFence returned */
   int result;    /* its WaitFence's, once fired */
};

static inline bool
darwin_any_same(const struct darwin_any_fence *a,
                const struct darwin_any_fence *b)
{
   return a->dev == b->dev && a->channel == b->channel && a->seqno == b->seqno;
}

/* lock held */
static inline void
darwin_any_put_locked(struct darwin_any_watcher *w)
{
   if (w != NULL && --w->refs == 0)
      free(w);
}

static inline void *
darwin_any_watch(void *arg)
{
   struct darwin_any_watcher *w = (struct darwin_any_watcher *)arg;
   const int result = w->ops->wait_fence(&w->fence, w->timeout_ns);
   pthread_mutex_lock(w->ops->lock);
   w->result = result;
   w->fired = true;
   pthread_cond_broadcast(w->ops->cond);
   pthread_mutex_t *lock = w->ops->lock;
   darwin_any_put_locked(w);
   pthread_mutex_unlock(lock);
   return NULL;
}

/* lock held: a watcher of fence, asleep until it passes or abs_timeout_ns */
static inline struct darwin_any_watcher *
darwin_any_start_locked(const struct darwin_any_ops *ops,
                        const struct darwin_any_fence *fence,
                        uint64_t abs_timeout_ns)
{
   struct darwin_any_watcher *w =
      (struct darwin_any_watcher *)calloc(1, sizeof(*w));
   if (w == NULL)
      return NULL;
   const uint64_t now = ops->now_ns();
   w->fence = *fence;
   w->timeout_ns = abs_timeout_ns == UINT64_MAX ? UINT64_MAX :
                   abs_timeout_ns > now ? abs_timeout_ns - now : 0;
   w->ops = ops;
   w->refs = 2;
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   const int err = pthread_create(&thread, &attr, darwin_any_watch, w);
   pthread_attr_destroy(&attr);
   if (err != 0) {
      free(w);
      return NULL;
   }
   return w;
}

/* lock held, after a sleep that reached its deadline: a watcher still asleep
 * on a fence that has passed gets the grace a wake may take, then is lost
 */
static inline void
darwin_any_check_wakes_locked(const struct darwin_any_ops *ops,
                              struct darwin_any_watcher **watchers,
                              uint32_t count)
{
   for (uint32_t i = 0; i < count; i++) {
      struct darwin_any_watcher *w = watchers[i];
      if (w == NULL || w->fired || !ops->fence_done(&w->fence))
         continue;
      const uint64_t grace = ops->now_ns() + DARWIN_ANY_WAKE_GRACE_NS;
      while (!w->fired && ops->cond_wait(grace) != ETIMEDOUT) {
      }
      if (!w->fired)
         ops->lost(&w->fence);
   }
}

/* lock held across: 0 once a wait of the set resolved, ETIMEDOUT at
 * abs_timeout_ns (UINT64_MAX: none), ENOMEM or a WaitFence's error
 */
static inline int
darwin_wait_any_locked(const struct darwin_any_ops *ops, void *ctx,
                       uint32_t count, uint64_t abs_timeout_ns)
{
   struct darwin_any_watcher **watchers =
      (struct darwin_any_watcher **)calloc(count ? count : 1, sizeof(*watchers));
   if (watchers == NULL)
      return ENOMEM;
   int ret = ETIMEDOUT;
   bool slept_out = false;
   for (;;) {
      /* past the deadline, only what resolved by now counts: no new watcher */
      const bool last = slept_out || (abs_timeout_ns != UINT64_MAX &&
                                      ops->now_ns() >= abs_timeout_ns);
      bool done = false;
      for (uint32_t i = 0; i < count && ret == ETIMEDOUT; i++) {
         struct darwin_any_fence fence;
         memset(&fence, 0, sizeof(fence));
         const enum darwin_any_status status = ops->status(ctx, i, &fence);
         if (status == DARWIN_ANY_DONE) {
            done = true;
            break;
         }
         struct darwin_any_watcher *w = watchers[i];
         const bool same = w != NULL && darwin_any_same(&w->fence, &fence);
         if (status != DARWIN_ANY_FENCE || !same) {
            darwin_any_put_locked(w);
            watchers[i] = w = NULL;
         }
         if (status != DARWIN_ANY_FENCE || (w != NULL && !w->fired))
            continue;
         if (w != NULL && w->result != 0 && w->result != ETIMEDOUT) {
            ret = w->result;
            break;
         }
         if (last)
            continue;
         darwin_any_put_locked(w);
         watchers[i] = darwin_any_start_locked(ops, &fence, abs_timeout_ns);
         if (watchers[i] == NULL)
            ret = ENOMEM;
      }
      if (done) {
         ret = 0;
         break;
      }
      if (ret != ETIMEDOUT || last)
         break;
      if (ops->cond_wait(abs_timeout_ns) == ETIMEDOUT) {
         darwin_any_check_wakes_locked(ops, watchers, count);
         slept_out = true;
      }
   }
   for (uint32_t i = 0; i < count; i++)
      darwin_any_put_locked(watchers[i]);
   free(watchers);
   return ret;
}

#endif
