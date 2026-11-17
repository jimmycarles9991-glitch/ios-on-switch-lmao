// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch UI stall reports (stall.c, stall_frame.c): the decisions, free of kernel types so kernel/tests can drive them.
 */
#ifndef SWITCH_DIAG_STALL_H
#define SWITCH_DIAG_STALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A main thread off core this long, other than idle in its run loop, is stalled. */
#define SWITCH_STALL_NS 2000000000ull

/* thread.h THREAD_NOT_RUNNABLE */
#define SWITCH_STALL_NOT_RUNNABLE (~0ull)

enum switch_stall_kind {
    SWITCH_STALL_NONE = 0,
    SWITCH_STALL_WAITING,  /* blocked */
    SWITCH_STALL_RUNNABLE, /* on a run queue, not run */
};

/* The scheduler's view of one thread, taken under its thread lock (mach absolute time). */
struct switch_stall_sample {
    bool waiting;       /* TH_WAIT without TH_RUN */
    bool runnable;      /* TH_RUN without TH_WAIT, on a run queue */
    bool running;       /* TH_RUN, on no run queue: on a core, or being dispatched to one */
    bool run_loop_idle; /* waiting in a receive on a port set: its run loop, idle */
    uint64_t now;
    uint64_t last_run;      /* thread->last_run_time: when it last left a core */
    uint64_t made_runnable; /* thread->last_made_runnable_time */
    uint64_t threshold;     /* SWITCH_STALL_NS in mach absolute time */
};

static inline enum switch_stall_kind switch_stall_kind(const struct switch_stall_sample *s)
{
    if (s->waiting && !s->run_loop_idle && s->now > s->last_run && s->now - s->last_run >= s->threshold) {
        return SWITCH_STALL_WAITING;
    }
    if (s->runnable && s->made_runnable != SWITCH_STALL_NOT_RUNNABLE && s->now > s->made_runnable &&
        s->now - s->made_runnable >= s->threshold) {
        return SWITCH_STALL_RUNNABLE;
    }
    return SWITCH_STALL_NONE;
}

/*
 * One stall episode per time the thread left a core: last_run_time only changes when it runs again, so a stall is
 * reported once however long it lasts, and its end is the first sample that finds it running or with a new
 * last_run_time.
 */
struct switch_stall_episode {
    bool open;
    uint64_t key;   /* last_run_time of the episode reported */
    uint64_t since; /* when it began: blocked (last_run_time) or made runnable */
    enum switch_stall_kind kind;
};

enum switch_stall_event {
    SWITCH_STALL_QUIET = 0,
    SWITCH_STALL_BEGAN,
    SWITCH_STALL_ENDED,
};

static inline enum switch_stall_event switch_stall_step(struct switch_stall_episode *e,
                                                        const struct switch_stall_sample *s)
{
    enum switch_stall_kind kind;

    if (e->open) {
        if (s->running || s->last_run != e->key) {
            e->open = false;
            return SWITCH_STALL_ENDED;
        }
        return SWITCH_STALL_QUIET;
    }
    kind = switch_stall_kind(s);
    if (kind == SWITCH_STALL_NONE || s->last_run == e->key) {
        return SWITCH_STALL_QUIET;
    }
    e->open = true;
    e->key = s->last_run;
    e->since = kind == SWITCH_STALL_RUNNABLE ? s->made_runnable : s->last_run;
    e->kind = kind;
    return SWITCH_STALL_BEGAN;
}

/*
 * What ran while a stall lasted: every thread's time on a core (mach absolute time) when its first line is written,
 * sorted by thread id, and at its end the threads that gained the most since. A thread the snapshot never saw began
 * after it, so all its time counts, unless the snapshot was cut short (`complete` false) and may have missed it.
 */
struct switch_stall_used {
    uint64_t tid;
    uint64_t cpu;
};

static inline int switch_stall_used_cmp(const void *a, const void *b)
{
    const uint64_t x = ((const struct switch_stall_used *)a)->tid, y = ((const struct switch_stall_used *)b)->tid;

    return x < y ? -1 : x > y ? 1 : 0;
}

static inline uint64_t switch_stall_gained(const struct switch_stall_used *seen, unsigned int n, bool complete,
                                           uint64_t tid, uint64_t cpu)
{
    unsigned int lo = 0, hi = n;

    while (lo < hi) {
        const unsigned int mid = lo + (hi - lo) / 2;

        if (seen[mid].tid == tid) {
            return cpu > seen[mid].cpu ? cpu - seen[mid].cpu : 0;
        }
        if (seen[mid].tid < tid) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return complete ? cpu : 0;
}

/* One of the threads that gained the most, `ref` the caller's handle for it. */
struct switch_stall_top {
    uint64_t tid;
    uint64_t cpu;
    void *ref;
};

/* Keeps top[0..k) the k largest gains, largest first; a gain of 0 never enters. */
static inline void switch_stall_rank(struct switch_stall_top *top, unsigned int k, struct switch_stall_top entry)
{
    unsigned int at = k;

    if (entry.cpu == 0) {
        return;
    }
    while (at > 0 && (top[at - 1].ref == NULL || top[at - 1].cpu < entry.cpu)) {
        at--;
    }
    if (at == k) {
        return;
    }
    for (unsigned int i = k - 1; i > at; i--) {
        top[i] = top[i - 1];
    }
    top[at] = entry;
}

/* block_hint.h's thread_snapshot_wait_flags, by value. */
static inline const char *switch_stall_wait_name(unsigned int hint)
{
    static const char *const names[] = {
        "no hint",
        "kernel mutex",
        "mach_msg receive",
        "mach_msg receive on a port set",
        "mach_msg send",
        "mach_msg send, in transit",
        "semaphore",
        "kernel rwlock read",
        "kernel rwlock write",
        "kernel rwlock upgrade",
        "ulock",
        "pthread mutex",
        "pthread rwlock read",
        "pthread rwlock write",
        "pthread condvar",
        "parked workqueue",
        "workloop sync wait",
        "wait4",
        "sleep with inheritor",
        "eventlink",
        "compressor",
    };
    return hint < sizeof(names) / sizeof(names[0]) ? names[hint] : "unknown wait";
}

#define SWITCH_STALL_WAIT_NAMES 21u

/* A user address as the host can name it: dyld shared cache (unslid) or main executable + offset. */
enum switch_stall_where {
    SWITCH_STALL_RAW = 0,
    SWITCH_STALL_DSC,
    SWITCH_STALL_MAIN,
};

static inline enum switch_stall_where switch_stall_user_where(uint64_t pc, uint64_t sr_base, uint64_t sr_size,
                                                              uint64_t sr_slide, uint64_t header, uint64_t *value)
{
    if (sr_size != 0 && pc >= sr_base && pc - sr_base < sr_size) {
        *value = pc - sr_slide;
        return SWITCH_STALL_DSC;
    }
    if (header != 0 && pc >= header && pc - header < (256ull << 20)) {
        *value = pc - header;
        return SWITCH_STALL_MAIN;
    }
    *value = pc;
    return SWITCH_STALL_RAW;
}

#endif /* SWITCH_DIAG_STALL_H */
