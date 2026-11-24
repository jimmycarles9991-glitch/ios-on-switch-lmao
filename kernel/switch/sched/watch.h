// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watched threads (watch.c, debug kernels): a client's thread that commits to the render server, and a backboardd
 * thread that sends a touch report on. For each event, the thread's time since the wake that ended its last idle wait
 * (its run loop's receive, or a parked work-queue thread's park), or since its previous event when it did not go idle
 * in between: on a core, blocked by kind with the longest waits, waiting for a core. busy.h keeps that stretch; this
 * adds the event that closes it and starts the next. A burst of messages (a commit sends one per context it changed,
 * back to back) is one event, its first message. Plain data and text, so kernel/tests runs it on the host; the
 * including file declares snprintf. Times are mach ticks.
 */
#ifndef SWITCH_SCHED_WATCH_H
#define SWITCH_SCHED_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "busy.h"

#define SWITCH_WATCH_NAME 17

/* block_hint.h: the waits a thread with nothing to do sits in */
#define SWITCH_WATCH_HINT_PARKED 0x0fu
#if defined(XNU_12377_121_6) && XNU_12377_121_6
#define SWITCH_WATCH_HINT_PARKED_BOUND 0x15u
#endif

enum {
    SWITCH_WATCH_COMMIT,
    SWITCH_WATCH_TOUCH,
    SWITCH_WATCH_KINDS,
    SWITCH_WATCH_BUSY = SWITCH_WATCH_KINDS /* a focal main thread's stretch between two run-loop waits: no counts */
};

/* What a late event's stretch went to most: on a core, waiting for a core, or blocked (busy.h's kinds). */
enum {
    SWITCH_WATCH_ON_CORE,
    SWITCH_WATCH_FOR_CORE,
    SWITCH_WATCH_BLOCKED,
    SWITCH_WATCH_CAUSES
};

struct switch_watch_thread {
    struct switch_busy busy;
    uint64_t last_event, last_message;
};

/* A timed event: the stretch that ended at it, and who and what it was (the caller's part). */
struct switch_watch_event {
    struct switch_busy_episode e;
    uint64_t gap;    /* since the thread's event before */
    bool from_event; /* the stretch began at that event, not at a wake from an idle wait */
    uint8_t kind;    /* SWITCH_WATCH_COMMIT, _TOUCH, _BUSY */
    uint8_t base_pri, sched_pri, mode, qos;
    int32_t pid, to_pid; /* touch: the receiver */
    uint64_t tid;
    uint64_t report; /* touch: the report's interrupt */
    char name[SWITCH_WATCH_NAME];
};

struct switch_watch_summary {
    uint64_t length;
    unsigned int cause;
};

enum switch_watch_step {
    SWITCH_WATCH_SAME_BURST, /* a later message of the event before */
    SWITCH_WATCH_FIRST,      /* the thread's first event: nothing before it to time */
    SWITCH_WATCH_TIMED,      /* *ev (when non-NULL) and *sum (when non-NULL) hold the stretch */
};

static inline bool switch_watch_idle(bool port_set, unsigned int hint)
{
#if defined(XNU_12377_121_6) && XNU_12377_121_6
    return port_set || hint == SWITCH_WATCH_HINT_PARKED || hint == SWITCH_WATCH_HINT_PARKED_BOUND;
#else
    return port_set || hint == SWITCH_WATCH_HINT_PARKED;
#endif
}

static inline uint64_t switch_watch_length(const struct switch_busy_episode *e)
{
    return e->end > e->woke ? e->end - e->woke : 0;
}

static inline unsigned int switch_watch_cause_of(uint64_t cpu, uint64_t blocked, uint64_t runnable)
{
    if (blocked >= cpu && blocked >= runnable) {
        return SWITCH_WATCH_BLOCKED;
    }
    return runnable > cpu ? SWITCH_WATCH_FOR_CORE : SWITCH_WATCH_ON_CORE;
}

static inline unsigned int switch_watch_cause(const struct switch_busy_episode *e)
{
    return switch_watch_cause_of(e->cpu, e->blocked, e->runnable);
}

/* A message at `now` from the thread, on a core, with CPU time `cpu`; `burst`: the longest gap inside one event.
 * `ev` and `sum` may each be NULL (e.g. a full ring that only needs `sum` to count the event). */
static inline enum switch_watch_step switch_watch_message(struct switch_watch_thread *t, uint64_t now, uint64_t cpu,
                                                          uint64_t burst, struct switch_watch_event *ev,
                                                          struct switch_watch_summary *sum)
{
    const bool same = t->last_message != 0 && now >= t->last_message && now - t->last_message <= burst;
    const bool had = t->busy.open && t->last_event != 0 && now >= t->last_event;

    t->last_message = now;
    if (same) {
        return SWITCH_WATCH_SAME_BURST;
    }
    if (had) {
        const uint64_t run_cpu = cpu >= t->busy.cpu0 ? cpu - t->busy.cpu0 : 0;

        if (sum != NULL) {
            sum->length = now > t->busy.e.woke ? now - t->busy.e.woke : 0;
            sum->cause = switch_watch_cause_of(run_cpu, t->busy.e.blocked, t->busy.e.runnable);
        }
        if (ev != NULL) {
            ev->e = t->busy.e;
            ev->e.end = now;
            ev->e.cpu = run_cpu;
            ev->gap = now - t->last_event;
            ev->from_event = t->busy.e.woke == t->last_event;
        }
    }
    __builtin_memset(&t->busy.e, 0, sizeof(t->busy.e));
    t->busy.open = true;
    t->busy.e.woke = now;
    t->busy.cpu0 = cpu;
    t->last_event = now;
    return had ? SWITCH_WATCH_TIMED : SWITCH_WATCH_FIRST;
}

/*
 * The focal app's launch: written at a role change (a seqlock: `seq` odd while one writer fills it), its first commit
 * taken by the first committer's compare-and-swap, `logged_seq` the writer's.
 */
struct switch_watch_launch {
    uint32_t seq;
    int32_t pid;
    char name[SWITCH_WATCH_NAME];
    uint64_t focal_at, first_commit;
    uint32_t logged_seq;
};

struct switch_watch_launch_snap {
    int32_t pid;
    char name[SWITCH_WATCH_NAME];
    uint64_t focal_at, first_commit;
};

/* Admit one writer (even -> odd). Acquire pairs with the previous writer's closing release store; the release fence
 * keeps the odd count before the payload stores for the reader's acquire fence. */
static inline bool switch_watch_launch_begin(struct switch_watch_launch *l, uint32_t *seq)
{
    uint32_t s = __atomic_load_n(&l->seq, __ATOMIC_RELAXED);

    if ((s & 1u) != 0 || !__atomic_compare_exchange_n(&l->seq, &s, s + 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return false;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *seq = s;
    return true;
}

static inline void switch_watch_launch_end(struct switch_watch_launch *l, uint32_t seq)
{
    __atomic_store_n(&l->seq, seq + 2u, __ATOMIC_RELEASE);
}

static inline bool switch_watch_launch_set(struct switch_watch_launch *l, int32_t pid, const char *name,
                                           uint64_t focal_at)
{
    uint32_t seq = 0;
    unsigned int i = 0;

    if (!switch_watch_launch_begin(l, &seq)) {
        return false;
    }
    __atomic_store_n(&l->pid, pid, __ATOMIC_RELAXED);
    for (; name != NULL && i + 1 < SWITCH_WATCH_NAME && name[i] != '\0'; i++) {
        __atomic_store_n(&l->name[i], name[i], __ATOMIC_RELAXED);
    }
    for (; i < SWITCH_WATCH_NAME; i++) {
        __atomic_store_n(&l->name[i], '\0', __ATOMIC_RELAXED);
    }
    __atomic_store_n(&l->focal_at, focal_at, __ATOMIC_RELAXED);
    __atomic_store_n(&l->first_commit, 0, __ATOMIC_RELAXED);
    switch_watch_launch_end(l, seq);
    return true;
}

static inline void switch_watch_launch_commit(struct switch_watch_launch *l, int32_t pid, uint64_t now)
{
    uint64_t zero = 0;

    if (__atomic_load_n(&l->pid, __ATOMIC_RELAXED) == pid) {
        __atomic_compare_exchange_n(&l->first_commit, &zero, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
}

static inline bool switch_watch_launch_take(struct switch_watch_launch *l, struct switch_watch_launch_snap *out)
{
    const uint32_t seq = __atomic_load_n(&l->seq, __ATOMIC_ACQUIRE);

    if ((seq & 1u) != 0 || seq == l->logged_seq) {
        return false;
    }
    out->pid = __atomic_load_n(&l->pid, __ATOMIC_RELAXED);
    for (unsigned int i = 0; i < SWITCH_WATCH_NAME; i++) {
        out->name[i] = __atomic_load_n(&l->name[i], __ATOMIC_RELAXED);
    }
    out->focal_at = __atomic_load_n(&l->focal_at, __ATOMIC_RELAXED);
    out->first_commit = __atomic_load_n(&l->first_commit, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (out->first_commit == 0 || __atomic_load_n(&l->seq, __ATOMIC_RELAXED) != seq) {
        return false;
    }
    l->logged_seq = seq;
    out->name[SWITCH_WATCH_NAME - 1] = '\0';
    return true;
}

/* Each watched thread's counts, cumulative, written by the thread alone (on a core, at its events). */
struct switch_watch_counts {
    uint32_t events[SWITCH_WATCH_KINDS];
    uint32_t late[SWITCH_WATCH_KINDS][SWITCH_WATCH_CAUSES];
    uint32_t dropped; /* late events no ring entry was free for */
};

static inline void switch_watch_count(struct switch_watch_counts *c, unsigned int kind, bool late, unsigned int cause)
{
    c->events[kind]++;
    if (late) {
        c->late[kind][cause]++;
    }
}

/* The window's sum over the threads: `now` less `was`, added to *sum. */
static inline void switch_watch_add(struct switch_watch_counts *sum, const struct switch_watch_counts *now,
                                    const struct switch_watch_counts *was)
{
    for (unsigned int k = 0; k < SWITCH_WATCH_KINDS; k++) {
        sum->events[k] += now->events[k] - was->events[k];
        for (unsigned int c = 0; c < SWITCH_WATCH_CAUSES; c++) {
            sum->late[k][c] += now->late[k][c] - was->late[k][c];
        }
    }
    sum->dropped += now->dropped - was->dropped;
}

static inline uint32_t switch_watch_late(const struct switch_watch_counts *c, unsigned int kind)
{
    return c->late[kind][SWITCH_WATCH_ON_CORE] + c->late[kind][SWITCH_WATCH_FOR_CORE] +
           c->late[kind][SWITCH_WATCH_BLOCKED];
}

/* "commits 812, 23 late (on core 15, waiting for a core 2, blocked 6); touch deliveries 120, 1 late (...)" */
static inline size_t switch_watch_counts_format(char *out, size_t size, const struct switch_watch_counts *c)
{
    static const char *const kinds[SWITCH_WATCH_KINDS] = {"commits", "touch deliveries"};
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    for (unsigned int k = 0; k < SWITCH_WATCH_KINDS; k++) {
        at = switch_busy_put(size, at,
                             snprintf(out + at, size - at,
                                      "%s%s %u, %u late (on core %u, waiting for a core %u, blocked %u)", k ? "; " : "",
                                      kinds[k], c->events[k], switch_watch_late(c, k), c->late[k][SWITCH_WATCH_ON_CORE],
                                      c->late[k][SWITCH_WATCH_FOR_CORE], c->late[k][SWITCH_WATCH_BLOCKED]));
    }
    return switch_busy_put(size, at, snprintf(out + at, size - at, "; %u late not logged", c->dropped));
}

/* A late event's text after the caller's head (process and thread): its length, where it began, the event before, its
 * main share, then busy.h's split. */
static inline size_t switch_watch_event_format(char *out, size_t size, const struct switch_watch_event *ev,
                                               uint32_t numer, uint32_t denom)
{
    static const char *const causes[SWITCH_WATCH_CAUSES] = {"on core", "waiting for a core", "blocked"};
    const uint64_t length = switch_busy_us(switch_watch_length(&ev->e), numer, denom);
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    at = switch_busy_put(size, at,
                         snprintf(out, size, "%llu.%llu ms since %s (the one before %llu ms earlier), mostly %s: ",
                                  (unsigned long long)(length / 1000), (unsigned long long)(length / 100 % 10),
                                  ev->from_event ? "its previous one" : "its idle wait ended",
                                  (unsigned long long)(switch_busy_us(ev->gap, numer, denom) / 1000),
                                  causes[switch_watch_cause(&ev->e)]));
    return switch_busy_put(size, at, switch_busy_format(out + at, size - at, &ev->e, numer, denom, 4));
}

#endif /* SWITCH_SCHED_WATCH_H */
