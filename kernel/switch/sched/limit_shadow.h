// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple's blocking per-thread CPU limit, replayed beside one this port lifted (<switch/cpulimit.h>; limit_shadow.c,
 * debug kernels). XNU's ledger refills a thread's allowance every refill interval; once the thread has run its budget
 * (the percentage of the interval) in an interval, it sleeps to the refill. Here it runs on: an interval in which it
 * passed its budget is a block Apple's limit would have made, and the time it ran past the budget is what that block
 * would have taken. Intervals start where the thread first ran and follow on whole, as the ledger's refills do. Plain
 * C, so kernel/tests runs it on the host. Times are mach ticks.
 *
 * The table: a slot per thread whose limit was lifted, holding its replay, and a row per process and limit holding
 * the counts the report reads. Every slot call comes from the thread itself with preemption off (a request, the end
 * of its lifted limit, its exit) or from its own dispatch hooks on the CPU it is going onto or leaving, so a slot has
 * one writer at a time; the CPU it runs on is the only writer of the row's counts for that CPU. A slot is freed when
 * its limit ends and claimed again by the next thread. Rows are keyed by process name, not pid, so a relaunch counts
 * on its old row, and they are never freed: a boot asks a handful of names and limits. Freeing one would race a thread
 * between choosing its row and claiming its slot.
 */
#ifndef SWITCH_SCHED_LIMIT_SHADOW_H
#define SWITCH_SCHED_LIMIT_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#ifndef SWITCH_LIMIT_CPUS
#define SWITCH_LIMIT_CPUS 4u
#endif
#define SWITCH_LIMIT_SLOTS 32u
#define SWITCH_LIMIT_ROWS  16u
#define SWITCH_LIMIT_NAME  17u

struct switch_limit_shadow {
    uint64_t interval, budget;
    uint64_t start, used; /* the current interval's start (0 before the first run) and the time run in it */
    uint64_t blocks, over;
};

static inline void switch_limit_shadow_retarget(struct switch_limit_shadow *s, uint64_t interval, uint8_t percentage)
{
    s->interval = interval ? interval : 1;
    s->budget = interval * percentage / 100u;
    s->start = s->used = 0;
}

static inline void switch_limit_shadow_init(struct switch_limit_shadow *s, uint64_t interval, uint8_t percentage)
{
    switch_limit_shadow_retarget(s, interval, percentage);
    s->blocks = s->over = 0;
}

/* The thread ran over [from, to). */
static inline void switch_limit_shadow_ran(struct switch_limit_shadow *s, uint64_t from, uint64_t to)
{
    while (from < to) {
        if (s->start == 0) {
            s->start = from;
            s->used = 0;
        } else if (from >= s->start + s->interval) {
            s->start += (from - s->start) / s->interval * s->interval;
            s->used = 0;
        }
        const uint64_t end = to < s->start + s->interval ? to : s->start + s->interval;
        const uint64_t before = s->used;

        s->used += end - from;
        if (before <= s->budget && s->used > s->budget) {
            s->blocks++;
        }
        if (s->used > s->budget) {
            s->over += s->used - (before > s->budget ? before : s->budget);
        }
        from = end;
    }
}

struct switch_limit_key {
    uint8_t percentage;
    uint64_t interval;
    uint32_t interval_ms;
    char name[SWITCH_LIMIT_NAME];
};

struct switch_limit_counts {
    uint64_t blocks, over;
};

enum {
    SWITCH_LIMIT_ROW_FREE,
    SWITCH_LIMIT_ROW_CLAIMING,
    SWITCH_LIMIT_ROW_READY
};

struct switch_limit_row {
    uint32_t state;
    struct switch_limit_key key;
    struct switch_limit_counts cpu[SWITCH_LIMIT_CPUS];
    struct switch_limit_counts seen; /* the report's */
};

#define SWITCH_LIMIT_SLOT_CLAIMING UINT64_MAX

struct switch_limit_slot {
    uint32_t row;
    uint64_t on_since; /* when the thread went on a core, 0 while off */
    struct switch_limit_shadow s;
};

/* The slots' thread ids sit apart from the slots: the dispatch hooks scan them on every context switch once a limit is
 * lifted, four cache lines. A slot's fields are written before its id is published (release). */
struct switch_limit_table {
    uint64_t tid[SWITCH_LIMIT_SLOTS]; /* 0: free */
    struct switch_limit_slot slots[SWITCH_LIMIT_SLOTS];
    struct switch_limit_row rows[SWITCH_LIMIT_ROWS];
    uint32_t watching, slots_dropped, rows_dropped;
};

/* Relaxed scan, acquire on the match. */
static inline struct switch_limit_slot *switch_limit_slot_of(struct switch_limit_table *t, uint64_t tid)
{
    for (unsigned int i = 0; i < SWITCH_LIMIT_SLOTS; i++) {
        if (__atomic_load_n(&t->tid[i], __ATOMIC_RELAXED) == tid &&
            __atomic_load_n(&t->tid[i], __ATOMIC_ACQUIRE) == tid) {
            return &t->slots[i];
        }
    }
    return 0;
}

static inline bool switch_limit_same_key(const struct switch_limit_key *a, const struct switch_limit_key *b)
{
    if (a->percentage != b->percentage || a->interval != b->interval) {
        return false;
    }
    for (unsigned int i = 0; i < SWITCH_LIMIT_NAME; i++) {
        if (a->name[i] != b->name[i]) {
            return false;
        }
        if (a->name[i] == 0) {
            return true;
        }
    }
    return true;
}

/* The row for a process name and limit, claimed when new; SWITCH_LIMIT_ROWS when the table is full. Two threads asking
 * the same new name and limit at once may each claim a row; the report prints both. */
static inline uint32_t switch_limit_row_for(struct switch_limit_table *t, const struct switch_limit_key *key)
{
    for (uint32_t i = 0; i < SWITCH_LIMIT_ROWS; i++) {
        if (__atomic_load_n(&t->rows[i].state, __ATOMIC_ACQUIRE) == SWITCH_LIMIT_ROW_READY &&
            switch_limit_same_key(&t->rows[i].key, key)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < SWITCH_LIMIT_ROWS; i++) {
        uint32_t expected = SWITCH_LIMIT_ROW_FREE;
        if (__atomic_compare_exchange_n(&t->rows[i].state, &expected, SWITCH_LIMIT_ROW_CLAIMING, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            t->rows[i].key = *key;
            __atomic_store_n(&t->rows[i].state, SWITCH_LIMIT_ROW_READY, __ATOMIC_RELEASE);
            return i;
        }
    }
    __atomic_fetch_add(&t->rows_dropped, 1, __ATOMIC_RELAXED);
    return SWITCH_LIMIT_ROWS;
}

/* The slot's thread ran over [from, to) on `cpu`: the replay, and what it added to the row's count for that CPU. */
static inline void switch_limit_run(struct switch_limit_table *t, struct switch_limit_slot *slot, unsigned int cpu,
                                    uint64_t from, uint64_t to)
{
    const uint64_t blocks = slot->s.blocks;
    const uint64_t over = slot->s.over;

    switch_limit_shadow_ran(&slot->s, from, to);
    if (cpu >= SWITCH_LIMIT_CPUS) {
        return;
    }
    struct switch_limit_counts *c = &t->rows[slot->row].cpu[cpu];
    __atomic_store_n(&c->blocks, __atomic_load_n(&c->blocks, __ATOMIC_RELAXED) + slot->s.blocks - blocks,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&c->over, __atomic_load_n(&c->over, __ATOMIC_RELAXED) + slot->s.over - over, __ATOMIC_RELAXED);
}

static inline void switch_limit_free(struct switch_limit_table *t, struct switch_limit_slot *slot)
{
    slot->on_since = 0;
    __atomic_store_n(&t->tid[slot - t->slots], 0, __ATOMIC_RELEASE);
}

/* A thread running at `now` had a blocking limit lifted. The same limit again changes nothing: the time since its
 * dispatch stays to be replayed when it leaves. A new limit replays that time under the old one first. */
static inline void switch_limit_requested(struct switch_limit_table *t, unsigned int cpu, uint64_t tid,
                                          const struct switch_limit_key *key, uint64_t now)
{
    struct switch_limit_slot *slot = switch_limit_slot_of(t, tid);
    const uint32_t row = switch_limit_row_for(t, key);

    if (slot != 0 && row == slot->row) {
        return;
    }
    if (slot != 0) {
        if (slot->on_since != 0 && now > slot->on_since) {
            switch_limit_run(t, slot, cpu, slot->on_since, now);
        }
        if (row == SWITCH_LIMIT_ROWS) {
            switch_limit_free(t, slot);
            return;
        }
        slot->row = row;
        slot->on_since = now;
        switch_limit_shadow_retarget(&slot->s, key->interval, key->percentage);
        return;
    }
    if (row == SWITCH_LIMIT_ROWS) {
        return;
    }
    for (unsigned int i = 0; i < SWITCH_LIMIT_SLOTS; i++) {
        uint64_t expected = 0;
        if (__atomic_load_n(&t->tid[i], __ATOMIC_RELAXED) == 0 &&
            __atomic_compare_exchange_n(&t->tid[i], &expected, SWITCH_LIMIT_SLOT_CLAIMING, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            slot = &t->slots[i];
            slot->row = row;
            slot->on_since = now;
            switch_limit_shadow_init(&slot->s, key->interval, key->percentage);
            __atomic_store_n(&t->tid[i], tid, __ATOMIC_RELEASE);
            __atomic_store_n(&t->watching, 1, __ATOMIC_RELAXED);
            return;
        }
    }
    __atomic_fetch_add(&t->slots_dropped, 1, __ATOMIC_RELAXED);
}

static inline void switch_limit_dispatched(struct switch_limit_table *t, uint64_t tid, uint64_t now)
{
    struct switch_limit_slot *slot = switch_limit_slot_of(t, tid);

    if (slot != 0) {
        slot->on_since = now;
    }
}

static inline void switch_limit_departed(struct switch_limit_table *t, unsigned int cpu, uint64_t tid, uint64_t now)
{
    struct switch_limit_slot *slot = switch_limit_slot_of(t, tid);

    if (slot != 0 && slot->on_since != 0 && now > slot->on_since) {
        switch_limit_run(t, slot, cpu, slot->on_since, now);
        slot->on_since = 0;
    }
}

/* The thread's lifted limit ended at `now`, while it runs: removed, replaced by one Apple's ledger applies, or the
 * thread exiting. Its run since dispatch is counted and the slot freed. */
static inline void switch_limit_ended(struct switch_limit_table *t, unsigned int cpu, uint64_t tid, uint64_t now)
{
    struct switch_limit_slot *slot = switch_limit_slot_of(t, tid);

    if (slot == 0) {
        return;
    }
    if (slot->on_since != 0 && now > slot->on_since) {
        switch_limit_run(t, slot, cpu, slot->on_since, now);
    }
    switch_limit_free(t, slot);
}

/* The report's: row `i`'s counts since the last call, and the threads holding a slot on it now. False for an unused
 * row. */
static inline bool switch_limit_window(struct switch_limit_table *t, uint32_t i, struct switch_limit_counts *window,
                                       unsigned int *threads)
{
    struct switch_limit_row *row = &t->rows[i];
    struct switch_limit_counts total = {0, 0};

    if (__atomic_load_n(&row->state, __ATOMIC_ACQUIRE) != SWITCH_LIMIT_ROW_READY) {
        return false;
    }
    for (unsigned int c = 0; c < SWITCH_LIMIT_CPUS; c++) {
        total.blocks += __atomic_load_n(&row->cpu[c].blocks, __ATOMIC_RELAXED);
        total.over += __atomic_load_n(&row->cpu[c].over, __ATOMIC_RELAXED);
    }
    window->blocks = total.blocks - row->seen.blocks;
    window->over = total.over - row->seen.over;
    row->seen = total;
    *threads = 0;
    for (unsigned int s = 0; s < SWITCH_LIMIT_SLOTS; s++) {
        const uint64_t tid = __atomic_load_n(&t->tid[s], __ATOMIC_ACQUIRE);
        if (tid != 0 && tid != SWITCH_LIMIT_SLOT_CLAIMING && t->slots[s].row == i) {
            (*threads)++;
        }
    }
    return true;
}

#endif /* SWITCH_SCHED_LIMIT_SHADOW_H */
