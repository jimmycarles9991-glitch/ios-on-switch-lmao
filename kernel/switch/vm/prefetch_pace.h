// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_PREFETCH_PACE_H
#define SWITCH_PREFETCH_PACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_PREFETCH_LIST_PAGE     4096u
#define SWITCH_PREFETCH_MAX_RUN_PAGES 128u
#define SWITCH_PREFETCH_MAX_GAP_PAGES 8u

/* Until sd-io adds a lower card class, replay runs in tier 0 beside the foreground with no priority override.
 * At boot 2's measured 87.8 MB/s replay throughput (278 KiB commands), 3 x 512 KiB (1.5 MiB) bounds the worst
 * foreground wait behind queued replay I/O to ~17 ms (~33 ms at 45 MB/s storm throughput) while keeping the card
 * fed across APFS hash-check completions. */
#define SWITCH_PREFETCH_INFLIGHT_LIMIT (3u * (uint64_t)SWITCH_PREFETCH_MAX_RUN_PAGES * SWITCH_PREFETCH_LIST_PAGE)

/* Shared lock-free pacing state for the single replay thread and its VM / UPL / pressure / shutdown wakers.
 * All counters are updated atomically so VM hooks under pmap_lock_phys_page, vm_page_queues_lock or
 * vm_object_lock take no locks and never block. */
struct switch_prefetch_pace {
    uint64_t inflight_bytes;
    uint64_t inflight_limit;
    uint64_t replayed_pages;
    uint64_t used_pages;
    uint64_t reclaimed_pages;
    uint64_t lead_bound;
    uint32_t active;
    uint32_t first_reclaim;
    uint32_t replay_stop;
    uint32_t boot_stop;
    uint32_t waiting;
};

static inline void switch_pace_init(struct switch_prefetch_pace *p, uint64_t list_vm_pages, uint64_t headroom_pages,
                                    uint64_t inflight_limit)
{
    uint64_t bound = headroom_pages < list_vm_pages ? headroom_pages : list_vm_pages;
    __atomic_store_n(&p->inflight_bytes, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&p->inflight_limit, inflight_limit, __ATOMIC_RELAXED);
    __atomic_store_n(&p->replayed_pages, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&p->used_pages, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&p->reclaimed_pages, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&p->lead_bound, bound, __ATOMIC_RELAXED);
    __atomic_store_n(&p->first_reclaim, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&p->waiting, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&p->active, bound > 0u ? 1u : 0u, __ATOMIC_RELEASE);
}

static inline void switch_pace_deactivate(struct switch_prefetch_pace *p)
{
    __atomic_store_n(&p->active, 0u, __ATOMIC_RELEASE);
}

static inline uint64_t switch_pace_inflight_bytes(const struct switch_prefetch_pace *p)
{
    return __atomic_load_n(&p->inflight_bytes, __ATOMIC_SEQ_CST);
}

static inline uint64_t switch_pace_lead(const struct switch_prefetch_pace *p)
{
    const uint64_t replayed = __atomic_load_n(&p->replayed_pages, __ATOMIC_SEQ_CST);
    const uint64_t used = __atomic_load_n(&p->used_pages, __ATOMIC_SEQ_CST);
    const uint64_t reclaimed = __atomic_load_n(&p->reclaimed_pages, __ATOMIC_SEQ_CST);
    const uint64_t retired = used + reclaimed;
    return replayed > retired ? replayed - retired : 0u;
}

static inline uint32_t switch_pace_replay_stop_reason(const struct switch_prefetch_pace *p)
{
    return __atomic_load_n(&p->replay_stop, __ATOMIC_SEQ_CST);
}

static inline uint32_t switch_pace_boot_stop_reason(const struct switch_prefetch_pace *p)
{
    return __atomic_load_n(&p->boot_stop, __ATOMIC_SEQ_CST);
}

static inline bool switch_pace_can_issue(const struct switch_prefetch_pace *p)
{
    if (__atomic_load_n(&p->replay_stop, __ATOMIC_SEQ_CST) != 0u) {
        return false;
    }
    const uint64_t inflight = __atomic_load_n(&p->inflight_bytes, __ATOMIC_SEQ_CST);
    const uint64_t limit = __atomic_load_n(&p->inflight_limit, __ATOMIC_RELAXED);
    if (inflight >= limit) {
        return false;
    }
    const uint64_t bound = __atomic_load_n(&p->lead_bound, __ATOMIC_SEQ_CST);
    return switch_pace_lead(p) < bound;
}

static inline void switch_pace_inflight_add(struct switch_prefetch_pace *p, uint64_t bytes)
{
    (void)__atomic_fetch_add(&p->inflight_bytes, bytes, __ATOMIC_SEQ_CST);
}

static inline bool switch_pace_inflight_done(struct switch_prefetch_pace *p, uint64_t bytes)
{
    uint64_t cur = __atomic_load_n(&p->inflight_bytes, __ATOMIC_SEQ_CST);
    uint64_t next = 0;
    for (;;) {
        next = cur > bytes ? cur - bytes : 0u;
        if (__atomic_compare_exchange_n(&p->inflight_bytes, &cur, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            break;
        }
    }
    return __atomic_load_n(&p->waiting, __ATOMIC_SEQ_CST) != 0u &&
           (next == 0u || __atomic_load_n(&p->replay_stop, __ATOMIC_SEQ_CST) != 0u || switch_pace_can_issue(p));
}

static inline void switch_pace_committed(struct switch_prefetch_pace *p, uint64_t pages)
{
    (void)__atomic_fetch_add(&p->replayed_pages, pages, __ATOMIC_SEQ_CST);
}

static inline bool switch_pace_on_used(struct switch_prefetch_pace *p)
{
    if (__builtin_expect(__atomic_load_n(&p->active, __ATOMIC_RELAXED) == 0u, 1)) {
        return false;
    }
    (void)__atomic_fetch_add(&p->used_pages, 1u, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&p->waiting, __ATOMIC_SEQ_CST) != 0u && switch_pace_can_issue(p);
}

static inline bool switch_pace_on_reclaimed(struct switch_prefetch_pace *p)
{
    if (__builtin_expect(__atomic_load_n(&p->active, __ATOMIC_RELAXED) == 0u, 1)) {
        return false;
    }
    (void)__atomic_fetch_add(&p->reclaimed_pages, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&p->replayed_pages, __ATOMIC_RELAXED) > 0u &&
        __atomic_load_n(&p->first_reclaim, __ATOMIC_RELAXED) == 0u) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&p->first_reclaim, &expected, 1u, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            const uint64_t lead = switch_pace_lead(p);
            const uint64_t clamped = lead > 0u ? lead : 1u;
            uint64_t cur = __atomic_load_n(&p->lead_bound, __ATOMIC_SEQ_CST);
            while (clamped < cur) {
                if (__atomic_compare_exchange_n(&p->lead_bound, &cur, clamped, false, __ATOMIC_SEQ_CST,
                                                __ATOMIC_SEQ_CST)) {
                    break;
                }
            }
        }
    }
    return __atomic_load_n(&p->waiting, __ATOMIC_SEQ_CST) != 0u && switch_pace_can_issue(p);
}

static inline bool switch_pace_stop_replay(struct switch_prefetch_pace *p, uint32_t reason, uint32_t *first_out)
{
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(&p->replay_stop, &expected, reason, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        if (first_out) {
            *first_out = reason;
        }
    } else if (first_out) {
        *first_out = expected;
    }
    return __atomic_load_n(&p->waiting, __ATOMIC_SEQ_CST) != 0u;
}

static inline bool switch_pace_stop_boot(struct switch_prefetch_pace *p, uint32_t reason, uint32_t *first_out)
{
    uint32_t replay_expected = 0u;
    (void)__atomic_compare_exchange_n(&p->replay_stop, &replay_expected, reason, false, __ATOMIC_SEQ_CST,
                                      __ATOMIC_SEQ_CST);
    uint32_t boot_expected = 0u;
    if (__atomic_compare_exchange_n(&p->boot_stop, &boot_expected, reason, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        if (first_out) {
            *first_out = reason;
        }
    } else if (first_out) {
        *first_out = boot_expected;
    }
    return __atomic_load_n(&p->waiting, __ATOMIC_SEQ_CST) != 0u;
}

#ifdef __cplusplus
}
#endif

#endif
