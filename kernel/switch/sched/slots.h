// SPDX-License-Identifier: GPL-2.0-only
/*
 * The watched threads' slot table (watch.c, debug kernels). Every context switch looks its threads up here, so the
 * lookup is one load of a 256-bit filter of the taken ids' hashes and, on a hit, a scan of one dense 512-byte array
 * of ids. tids[i] is 0 while slot i is free, SWITCH_SLOT_CLAIMING while its new owner sets it up, then the owner's id.
 * Claims only set filter bits; the 10 s report, the only remover, frees slots and clears the bits no id needs.
 * Builtin atomics, so kernel/tests runs it on the host.
 */
#ifndef SWITCH_SCHED_SLOTS_H
#define SWITCH_SCHED_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_SLOTS         64u
#define SWITCH_SLOT_WORDS    4u
#define SWITCH_SLOT_CLAIMING UINT64_MAX

struct switch_slots {
    uint64_t tids[SWITCH_SLOTS];
    uint64_t filter[SWITCH_SLOT_WORDS];
    uint64_t refused[SWITCH_SLOT_WORDS]; /* hashes of threads refused a slot since the report: not tried again */
};

static inline unsigned int switch_slot_hash(uint64_t tid)
{
    return (unsigned int)((tid * 0x9e3779b97f4a7c15ull) >> 56);
}

static inline uint64_t switch_slot_bit(unsigned int hash)
{
    return 1ull << (hash & 63u);
}

static inline bool switch_slot_taken(uint64_t tid)
{
    return tid != 0 && tid != SWITCH_SLOT_CLAIMING;
}

/*
 * The slot of thread `tid`, or -1. The scan's loads are relaxed; a match is followed by an acquire fence, which pairs
 * with switch_slot_publish's release store of the id, so the slot's set-up is seen with it.
 */
static inline int switch_slot_find(const struct switch_slots *s, uint64_t tid)
{
    const unsigned int hash = switch_slot_hash(tid);

    if ((__atomic_load_n(&s->filter[hash >> 6], __ATOMIC_RELAXED) & switch_slot_bit(hash)) == 0) {
        return -1;
    }
    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        if (__atomic_load_n(&s->tids[i], __ATOMIC_RELAXED) == tid) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return (int)i;
        }
    }
    return -1;
}

/*
 * A free slot for thread `tid`, or -1. The compare-and-swap from free takes the slot without a lock; its acquire keeps
 * the caller's set-up stores after the claim. If `tid` already holds a slot (a transiently cleared filter bit during a
 * report), its filter bit is restored and -1 is returned with *first false so no duplicate slot is claimed. A thread
 * refused once (a full table) is not tried again until the report clears the refused set, and *first says whether this
 * refusal was its first in the window.
 */
static inline int switch_slot_claim(struct switch_slots *s, uint64_t tid, bool *first)
{
    const unsigned int hash = switch_slot_hash(tid);
    int empty = -1;

    *first = false;
    if ((__atomic_load_n(&s->refused[hash >> 6], __ATOMIC_RELAXED) & switch_slot_bit(hash)) != 0) {
        return -1;
    }
    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        const uint64_t cur = __atomic_load_n(&s->tids[i], __ATOMIC_RELAXED);

        if (cur == tid) {
            __atomic_fetch_or(&s->filter[hash >> 6], switch_slot_bit(hash), __ATOMIC_RELEASE);
            return -1;
        }
        if (cur == 0 && empty < 0) {
            empty = (int)i;
        }
    }
    if (empty >= 0) {
        for (unsigned int i = (unsigned int)empty; i < SWITCH_SLOTS; i++) {
            uint64_t free_id = 0;

            if (__atomic_load_n(&s->tids[i], __ATOMIC_RELAXED) == 0 &&
                __atomic_compare_exchange_n(&s->tids[i], &free_id, SWITCH_SLOT_CLAIMING, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return (int)i;
            }
        }
    }
    *first = (__atomic_fetch_or(&s->refused[hash >> 6], switch_slot_bit(hash), __ATOMIC_RELAXED) &
              switch_slot_bit(hash)) == 0;
    return -1;
}

/*
 * Slot `i`, claimed for `tid`, is set up: its id is stored with release (the set-up before it), then its filter bit
 * is set with release, which a report clearing bits acquires (switch_slot_free_gone).
 */
static inline void switch_slot_publish(struct switch_slots *s, unsigned int i, uint64_t tid)
{
    const unsigned int hash = switch_slot_hash(tid);

    __atomic_store_n(&s->tids[i], tid, __ATOMIC_RELEASE);
    __atomic_fetch_or(&s->filter[hash >> 6], switch_slot_bit(hash), __ATOMIC_RELEASE);
}

/* The ids taken when the report began, with their hashes. */
struct switch_slot_snap {
    uint64_t tids[SWITCH_SLOTS];
    uint64_t hashes[SWITCH_SLOT_WORDS];
};

/* The report, before it walks the system's threads: the ids taken now (one being claimed is not one yet). */
static inline void switch_slot_snapshot(const struct switch_slots *s, struct switch_slot_snap *snap)
{
    __builtin_memset(snap, 0, sizeof(*snap));
    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        const uint64_t tid = __atomic_load_n(&s->tids[i], __ATOMIC_RELAXED);

        if (switch_slot_taken(tid)) {
            snap->tids[i] = tid;
            snap->hashes[switch_slot_hash(tid) >> 6] |= switch_slot_bit(switch_slot_hash(tid));
        }
    }
}

/* The slot `tid` held in the snapshot, or -1. */
static inline int switch_slot_snapshot_find(const struct switch_slot_snap *snap, uint64_t tid)
{
    const unsigned int hash = switch_slot_hash(tid);

    if ((snap->hashes[hash >> 6] & switch_slot_bit(hash)) == 0) {
        return -1;
    }
    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        if (snap->tids[i] == tid) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * After the walk: each snapshot slot whose thread was not seen (`alive`) is freed by compare-and-swap from its
 * snapshot id, so a slot taken after the snapshot is never freed under its thread; the release keeps the report's
 * reads of the slot before a new owner can take it. Then the filter bits no taken id needs are cleared, and the
 * refused set. Returns the slots freed.
 */
static inline unsigned int switch_slot_free_gone(struct switch_slots *s, const struct switch_slot_snap *snap,
                                                 const bool alive[SWITCH_SLOTS])
{
    uint64_t keep[SWITCH_SLOT_WORDS] = {0};
    unsigned int freed = 0;

    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        uint64_t tid = snap->tids[i];

        if (tid != 0 && !alive[i] &&
            __atomic_compare_exchange_n(&s->tids[i], &tid, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            freed++;
        }
    }
    if (freed != 0) {
        for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
            const uint64_t tid = __atomic_load_n(&s->tids[i], __ATOMIC_RELAXED);

            if (switch_slot_taken(tid)) {
                keep[switch_slot_hash(tid) >> 6] |= switch_slot_bit(switch_slot_hash(tid));
            }
        }
        for (unsigned int w = 0; w < SWITCH_SLOT_WORDS; w++) {
            const uint64_t stale = __atomic_load_n(&s->filter[w], __ATOMIC_RELAXED) & ~keep[w];

            /* acq_rel: a publish whose bit-set came before this clear in the word's order is acquired here, so the
             * re-scan below sees its id and sets its bit again; one that comes after sets it itself. */
            if (stale != 0) {
                __atomic_fetch_and(&s->filter[w], ~stale, __ATOMIC_ACQ_REL);
            }
        }
        for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
            const uint64_t tid = __atomic_load_n(&s->tids[i], __ATOMIC_RELAXED);

            if (switch_slot_taken(tid)) {
                __atomic_fetch_or(&s->filter[switch_slot_hash(tid) >> 6], switch_slot_bit(switch_slot_hash(tid)),
                                  __ATOMIC_RELAXED);
            }
        }
    }
    for (unsigned int w = 0; w < SWITCH_SLOT_WORDS; w++) {
        __atomic_store_n(&s->refused[w], 0, __ATOMIC_RELAXED);
    }
    return freed;
}

static inline bool switch_slot_free_tid(struct switch_slots *s, uint64_t tid)
{
    const int i = switch_slot_find(s, tid);

    if (i < 0) {
        return false;
    }
    uint64_t expected = tid;
    return __atomic_compare_exchange_n(&s->tids[i], &expected, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static inline void switch_slot_prune_filter(struct switch_slots *s)
{
    uint64_t keep[SWITCH_SLOT_WORDS] = {0};

    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        const uint64_t tid = __atomic_load_n(&s->tids[i], __ATOMIC_RELAXED);

        if (switch_slot_taken(tid)) {
            keep[switch_slot_hash(tid) >> 6] |= switch_slot_bit(switch_slot_hash(tid));
        }
    }
    for (unsigned int w = 0; w < SWITCH_SLOT_WORDS; w++) {
        const uint64_t stale = __atomic_load_n(&s->filter[w], __ATOMIC_RELAXED) & ~keep[w];

        if (stale != 0) {
            __atomic_fetch_and(&s->filter[w], ~stale, __ATOMIC_ACQ_REL);
        }
    }
    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        const uint64_t tid = __atomic_load_n(&s->tids[i], __ATOMIC_RELAXED);

        if (switch_slot_taken(tid)) {
            __atomic_fetch_or(&s->filter[switch_slot_hash(tid) >> 6], switch_slot_bit(switch_slot_hash(tid)),
                              __ATOMIC_RELAXED);
        }
    }
    for (unsigned int w = 0; w < SWITCH_SLOT_WORDS; w++) {
        __atomic_store_n(&s->refused[w], 0, __ATOMIC_RELAXED);
    }
}

#endif /* SWITCH_SCHED_SLOTS_H */
