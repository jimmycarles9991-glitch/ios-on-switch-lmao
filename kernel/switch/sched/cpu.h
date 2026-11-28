// SPDX-License-Identifier: GPL-2.0-only
/*
 * Who used the CPU in a window (cpu.c, debug kernels). Each core's time splits into its threads' time and its idle
 * thread's, and the idle thread's into asleep (WFI and C7), interrupts taken while idle, and the rest (the idle loop's
 * own path). Each process's user and system time comes from the scheduler's lifetime counts (recount), taken at the
 * report and at the process's exit. Plain data and text, free of kernel types, so kernel/tests runs it on the host; the
 * including file declares snprintf. Times are mach ticks.
 *
 * A process is a row keyed by its unique id: an exec keeps the id and carries the old task's counts into the new one
 * (task_rollup_accounting_info), so a process stays one row across an exec. Each window fills one table while the
 * other holds the counts the last window ended with; a process first taken in a window starts from the other table's
 * row, or from zero when it had none (it began in this window).
 */
#ifndef SWITCH_SCHED_CPU_H
#define SWITCH_SCHED_CPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_CPU_ROWS   1024u /* a power of two, well over the ~350 processes of a busy boot */
#define SWITCH_CPU_PROBES 32u
#define SWITCH_CPU_TOP    8u
#define SWITCH_CPU_NAME   17u
#define SWITCH_CPU_CORES  8u

struct switch_cpu_row {
    uint64_t uid; /* 0: empty */
    uint64_t user, sys;
};

struct switch_cpu_task {
    int32_t pid;
    char name[SWITCH_CPU_NAME];
    uint64_t user, sys;
};

struct switch_cpu_tasks {
    struct switch_cpu_row rows[2][SWITCH_CPU_ROWS];
    uint32_t cur;
    uint32_t untracked; /* takes no row had room for: their time is in no figure */
    uint32_t exits;
    uint64_t exit_user, exit_sys;
    struct switch_cpu_task most_exited;
};

static inline uint32_t switch_cpu_hash(uint64_t uid)
{
    return (uint32_t)((uid * 0x9e3779b97f4a7c15ull) >> 54) & (SWITCH_CPU_ROWS - 1);
}

static inline struct switch_cpu_row *switch_cpu_find(struct switch_cpu_row *table, uint64_t uid, bool insert)
{
    uint32_t i = switch_cpu_hash(uid);

    for (uint32_t probe = 0; probe < SWITCH_CPU_PROBES; probe++, i = (i + 1) & (SWITCH_CPU_ROWS - 1)) {
        if (table[i].uid == uid) {
            return &table[i];
        }
        if (table[i].uid == 0) {
            if (!insert) {
                return NULL;
            }
            table[i].uid = uid;
            table[i].user = table[i].sys = 0;
            return &table[i];
        }
    }
    return NULL;
}

/* The process `uid` has used `user` and `sys` in its life: what it used since it was last taken goes to *du, *ds. False
 * (and nothing) when its row had no room. */
static inline bool switch_cpu_take(struct switch_cpu_tasks *t, uint64_t uid, uint64_t user, uint64_t sys, uint64_t *du,
                                   uint64_t *ds)
{
    struct switch_cpu_row *row = switch_cpu_find(t->rows[t->cur], uid, false);

    if (row == NULL) {
        const struct switch_cpu_row *was = switch_cpu_find(t->rows[t->cur ^ 1], uid, false);

        row = switch_cpu_find(t->rows[t->cur], uid, true);
        if (row == NULL) {
            t->untracked++;
            *du = *ds = 0;
            return false;
        }
        if (was != NULL) {
            row->user = was->user;
            row->sys = was->sys;
        }
    }
    *du = user > row->user ? user - row->user : 0;
    *ds = sys > row->sys ? sys - row->sys : 0;
    row->user = user > row->user ? user : row->user;
    row->sys = sys > row->sys ? sys : row->sys;
    return true;
}

static inline void switch_cpu_name(char *to, const char *from)
{
    uint32_t i = 0;

    for (; i + 1 < SWITCH_CPU_NAME && from != NULL && from[i] != '\0'; i++) {
        to[i] = from[i];
    }
    to[i] = '\0';
}

/* A process exits: what it used since it was last taken is the window's exited time. */
static inline void switch_cpu_exited(struct switch_cpu_tasks *t, uint64_t uid, int32_t pid, const char *name,
                                     uint64_t user, uint64_t sys)
{
    uint64_t du = 0, ds = 0;

    if (!switch_cpu_take(t, uid, user, sys, &du, &ds)) {
        return;
    }
    t->exits++;
    t->exit_user += du;
    t->exit_sys += ds;
    if (du + ds > t->most_exited.user + t->most_exited.sys) {
        t->most_exited.pid = pid;
        t->most_exited.user = du;
        t->most_exited.sys = ds;
        switch_cpu_name(t->most_exited.name, name);
    }
}

/* The window's exits as they stand, and a new window: the table just filled holds the counts it ends with. */
struct switch_cpu_exits {
    uint32_t exits, untracked;
    uint64_t user, sys;
    struct switch_cpu_task most;
};

static inline struct switch_cpu_exits switch_cpu_end_window(struct switch_cpu_tasks *t)
{
    const struct switch_cpu_exits e = {t->exits, t->untracked, t->exit_user, t->exit_sys, t->most_exited};

    t->cur ^= 1;
    __builtin_memset(t->rows[t->cur], 0, sizeof(t->rows[t->cur]));
    t->exits = t->untracked = 0;
    t->exit_user = t->exit_sys = 0;
    __builtin_memset(&t->most_exited, 0, sizeof(t->most_exited));
    return e;
}

/* The top SWITCH_CPU_TOP by user + system, most first: the slot a process with `ticks` takes, or -1. */
static inline int switch_cpu_rank(const struct switch_cpu_task *top, uint64_t ticks)
{
    if (ticks == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < SWITCH_CPU_TOP; i++) {
        if (ticks > top[i].user + top[i].sys) {
            return (int)i;
        }
    }
    return -1;
}

static inline void switch_cpu_place(struct switch_cpu_task *top, int at, const struct switch_cpu_task *task)
{
    for (int j = (int)SWITCH_CPU_TOP - 1; j > at; j--) {
        top[j] = top[j - 1];
    }
    top[at] = *task;
}

/* One core's window: its threads' time (interrupts while they ran included) and its idle thread's, split. */
struct switch_cpu_core {
    uint64_t threads, irq;   /* non-idle threads' user + system; every interrupt the core took */
    uint64_t idle_irq;       /* interrupts taken while the core idled */
    uint64_t asleep, sleeps; /* in WFI or C7, and the entries into them */
};

static inline uint64_t switch_cpu_ms(uint64_t ticks, uint32_t numer, uint32_t denom)
{
    return ticks * numer / denom / 1000000u;
}

static inline size_t switch_cpu_put(size_t size, size_t at, int n)
{
    if (n < 0 || at >= size) {
        return at;
    }
    return at + (size_t)n < size ? at + (size_t)n : size - 1;
}

/*
 * "cpu0 threads 5600 (irq 150), idle 4400 = asleep 4100 + irq 150 + rest 150, 12000 sleeps; ..." in ms over a
 * `window` of mach ticks. The rest is the idle thread's own path between sleeps: scheduling, timers, the C7 exits.
 */
static inline size_t switch_cpu_cores_format(char *out, size_t size, const struct switch_cpu_core *cores,
                                             uint32_t count, uint64_t window, uint32_t numer, uint32_t denom)
{
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    for (uint32_t c = 0; c < count; c++) {
        const struct switch_cpu_core *k = &cores[c];
        const uint64_t idle = window > k->threads ? window - k->threads : 0;
        const uint64_t accounted = k->asleep + k->idle_irq;
        const uint64_t threads_irq = k->irq > k->idle_irq ? k->irq - k->idle_irq : 0;
        const uint64_t rest = idle > accounted ? idle - accounted : 0;

        at = switch_cpu_put(size, at,
                            snprintf(out + at, size - at,
                                     "%scpu%u threads %llu (irq %llu), idle %llu = asleep %llu + irq %llu + rest %llu, "
                                     "%llu sleeps",
                                     c ? "; " : "", c, (unsigned long long)switch_cpu_ms(k->threads, numer, denom),
                                     (unsigned long long)switch_cpu_ms(threads_irq, numer, denom),
                                     (unsigned long long)switch_cpu_ms(idle, numer, denom),
                                     (unsigned long long)switch_cpu_ms(k->asleep, numer, denom),
                                     (unsigned long long)switch_cpu_ms(k->idle_irq, numer, denom),
                                     (unsigned long long)switch_cpu_ms(rest, numer, denom),
                                     (unsigned long long)k->sleeps));
    }
    return at;
}

/* "SpringBoard[393] 1200+300, ...; all 312 processes 3900+1800, exited 12 (410+30, most prngseedd[1234] 80+2),
 * untracked 0" in ms, user+system. */
static inline size_t switch_cpu_tasks_format(char *out, size_t size, const struct switch_cpu_task *top,
                                             uint32_t processes, uint64_t all_user, uint64_t all_sys,
                                             const struct switch_cpu_exits *e, uint32_t numer, uint32_t denom)
{
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    for (uint32_t i = 0; i < SWITCH_CPU_TOP && top[i].user + top[i].sys != 0; i++) {
        at = switch_cpu_put(size, at,
                            snprintf(out + at, size - at, "%s%s[%d] %llu+%llu", i ? ", " : "", top[i].name, top[i].pid,
                                     (unsigned long long)switch_cpu_ms(top[i].user, numer, denom),
                                     (unsigned long long)switch_cpu_ms(top[i].sys, numer, denom)));
    }
    at = switch_cpu_put(size, at,
                        snprintf(out + at, size - at, "%sall %u processes %llu+%llu, exited %u (%llu+%llu",
                                 at ? "; " : "", processes, (unsigned long long)switch_cpu_ms(all_user, numer, denom),
                                 (unsigned long long)switch_cpu_ms(all_sys, numer, denom), e->exits,
                                 (unsigned long long)switch_cpu_ms(e->user, numer, denom),
                                 (unsigned long long)switch_cpu_ms(e->sys, numer, denom)));
    if (e->exits != 0) {
        at = switch_cpu_put(size, at,
                            snprintf(out + at, size - at, ", most %s[%d] %llu+%llu", e->most.name, e->most.pid,
                                     (unsigned long long)switch_cpu_ms(e->most.user, numer, denom),
                                     (unsigned long long)switch_cpu_ms(e->most.sys, numer, denom)));
    }
    return switch_cpu_put(size, at, snprintf(out + at, size - at, "), untracked %u", e->untracked));
}

/* Most user + system first (insertion sort: a boot's few hundred processes, once). */
static inline void switch_cpu_tasks_sort(struct switch_cpu_task *tasks, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++) {
        const struct switch_cpu_task t = tasks[i];
        uint32_t j = i;

        for (; j > 0 && tasks[j - 1].user + tasks[j - 1].sys < t.user + t.sys; j--) {
            tasks[j] = tasks[j - 1];
        }
        tasks[j] = t;
    }
}

/* "SpringBoard[30] 1200+300, ..." from tasks[first] on, as many as fit in `size`; returns the first one left out. */
static inline uint32_t switch_cpu_totals_format(char *out, size_t size, const struct switch_cpu_task *tasks,
                                                uint32_t count, uint32_t first, uint32_t numer, uint32_t denom)
{
    size_t at = 0;
    uint32_t i = first;

    if (size == 0) {
        return first;
    }
    out[0] = '\0';
    for (; i < count; i++) {
        char entry[64];
        const int n = snprintf(entry, sizeof(entry), "%s%s[%d] %llu+%llu", i > first ? ", " : "", tasks[i].name,
                               tasks[i].pid, (unsigned long long)switch_cpu_ms(tasks[i].user, numer, denom),
                               (unsigned long long)switch_cpu_ms(tasks[i].sys, numer, denom));

        if (n < 0 || at + (size_t)n >= size) {
            break;
        }
        __builtin_memcpy(out + at, entry, (size_t)n + 1);
        at += (size_t)n;
    }
    return i;
}

#endif /* SWITCH_SCHED_CPU_H */
