// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_PREFETCH_QUIESCE_H
#define SWITCH_PREFETCH_QUIESCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_QUIESCE_STOPPING 0x80000000u

/* Lock-free writer-in-flight gate for the wired recording log.
 * Writers increment `active` (seq_cst) before reloading `ptr` (seq_cst), while stop clears `ptr` (seq_cst)
 * before reading `active` (seq_cst). Total order guarantees that once `active` drains to 0 after `ptr` is NULL,
 * no writer still holds a pointer to the log and the caller can safely free it. */
struct switch_quiesce {
    void *ptr;
    uint32_t active;
};

typedef void (*switch_quiesce_wake_fn)(void *arg);
typedef void (*switch_quiesce_wait_fn)(struct switch_quiesce *q, void *arg);

static inline void switch_quiesce_init(struct switch_quiesce *q)
{
    __atomic_store_n(&q->ptr, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&q->active, 0u, __ATOMIC_RELAXED);
}

/* One-shot per boot (or strictly after a completed switch_quiesce_stop): active is already 0, so leave it
 * untouched rather than risking an underflow against any concurrent rollback. */
static inline void switch_quiesce_start(struct switch_quiesce *q, void *ptr)
{
    __atomic_store_n(&q->ptr, ptr, __ATOMIC_RELEASE);
}

static inline bool switch_quiesce_is_armed(const struct switch_quiesce *q)
{
    return __atomic_load_n(&q->ptr, __ATOMIC_RELAXED) != NULL;
}

static inline void switch_quiesce_leave(struct switch_quiesce *q, switch_quiesce_wake_fn wake, void *arg)
{
    const uint32_t prev = __atomic_fetch_sub(&q->active, 1u, __ATOMIC_RELEASE);
    const uint32_t after = prev - 1u;
    if ((after & ~SWITCH_QUIESCE_STOPPING) == 0u && (after & SWITCH_QUIESCE_STOPPING) != 0u && wake != NULL) {
        wake(arg);
    }
}

static inline void *switch_quiesce_enter(struct switch_quiesce *q, switch_quiesce_wake_fn wake, void *arg)
{
    if (__atomic_load_n(&q->ptr, __ATOMIC_RELAXED) == NULL) {
        return NULL;
    }
    (void)__atomic_fetch_add(&q->active, 1u, __ATOMIC_SEQ_CST);
    void *ptr = __atomic_load_n(&q->ptr, __ATOMIC_SEQ_CST);
    if (ptr == NULL) {
        switch_quiesce_leave(q, wake, arg);
        return NULL;
    }
    return ptr;
}

static inline bool switch_quiesce_has_writers(const struct switch_quiesce *q)
{
    return (__atomic_load_n(&q->active, __ATOMIC_SEQ_CST) & ~SWITCH_QUIESCE_STOPPING) != 0u;
}

static inline bool switch_quiesce_mark_stopping(struct switch_quiesce *q)
{
    const uint32_t prev = __atomic_fetch_or(&q->active, SWITCH_QUIESCE_STOPPING, __ATOMIC_ACQ_REL);
    return (prev & ~SWITCH_QUIESCE_STOPPING) != 0u;
}

static inline void *switch_quiesce_stop(struct switch_quiesce *q, switch_quiesce_wait_fn wait, void *arg)
{
    void *old = __atomic_exchange_n(&q->ptr, NULL, __ATOMIC_SEQ_CST);
    if (old == NULL) {
        return NULL;
    }
    if (switch_quiesce_has_writers(q) && wait != NULL) {
        wait(q, arg);
    }
    __atomic_and_fetch(&q->active, ~SWITCH_QUIESCE_STOPPING, __ATOMIC_RELAXED);
    return old;
}

#ifdef __cplusplus
}
#endif

#endif
