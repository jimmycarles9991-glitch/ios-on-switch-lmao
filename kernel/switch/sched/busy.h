// SPDX-License-Identifier: GPL-2.0-only
/*
 * A UI main thread's busy stretches (busy.c, debug kernels). A stretch runs from the wake that ends a wait in its run loop
 * (a mach_msg receive on a port set) to its next such wait. The thread's own departures from a core and dispatches onto
 * one split it into on a core, blocked (by wait kind) and waiting for a core, and keep its longest waits. A nested run
 * loop's wait ends a stretch too. Plain data and text, so kernel/tests runs it on the host; the including file declares
 * snprintf. Times are mach ticks.
 */
#ifndef SWITCH_SCHED_BUSY_H
#define SWITCH_SCHED_BUSY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../diag/stall.h"

#define SWITCH_BUSY_LONGEST 3
#define SWITCH_BUSY_HINTS   32u /* block_hint.h's kThreadWait* values below this are counted by kind */

/* One wait: block_hint.h's hint, the user PC and LR the thread trapped from (unslid, switch_stall_user_where), the
 * kernel continuation it parked in (unslid, 0 when it blocked on its stack) and the wait event. */
struct switch_busy_wait {
    uint64_t ticks;
    uint64_t pc, lr, kernel, event;
    uint8_t pc_where, lr_where;
    uint8_t hint;
};

struct switch_busy_episode {
    uint64_t woke, end; /* made runnable out of its run loop's wait; back in it */
    uint64_t cpu, blocked, runnable;
    uint32_t waits, preemptions;
    uint64_t hint_ticks[SWITCH_BUSY_HINTS];
    uint32_t hint_waits[SWITCH_BUSY_HINTS];
    struct switch_busy_wait longest[SWITCH_BUSY_LONGEST];
};

/* One thread's state, written only by its own departures and dispatches (never two at once: a thread is dispatched
 * only after its departure's thread_dispatch). */
struct switch_busy {
    bool seen;
    bool open;
    bool off_waiting; /* its last departure was into a wait other than its run loop's */
    bool off_idle;    /* its last departure was into its run loop's wait */
    uint64_t off, off_cpu, cpu0;
    struct switch_busy_wait pending;
    struct switch_busy_episode e;
};

static inline void switch_busy_keep_longest(struct switch_busy_episode *e, const struct switch_busy_wait *w)
{
    for (unsigned int i = 0; i < SWITCH_BUSY_LONGEST; i++) {
        if (w->ticks > e->longest[i].ticks) {
            for (unsigned int j = SWITCH_BUSY_LONGEST - 1; j > i; j--) {
                e->longest[j] = e->longest[j - 1];
            }
            e->longest[i] = *w;
            return;
        }
    }
}

/*
 * The thread leaves a core at `now` with CPU time `cpu`: into a wait (`waiting`), its run loop's in particular (`idle`),
 * or still runnable (preempted or yielding). A stretch of `min` or more ending here is copied to *done and true returned.
 */
static inline bool switch_busy_departed(struct switch_busy *b, uint64_t now, uint64_t cpu, bool waiting, bool idle,
                                        const struct switch_busy_wait *wait, uint64_t min,
                                        struct switch_busy_episode *done)
{
    bool ended = false;

    if (b->open && waiting && idle) {
        b->open = false;
        if (now > b->e.woke && now - b->e.woke >= min) {
            *done = b->e;
            done->end = now;
            done->cpu = cpu >= b->cpu0 ? cpu - b->cpu0 : 0;
            ended = true;
        }
    }
    b->seen = true;
    b->off = now;
    b->off_cpu = cpu;
    b->off_waiting = waiting && !idle;
    b->off_idle = waiting && idle;
    if (b->off_waiting) {
        b->pending = *wait;
    }
    return ended;
}

/* The thread is put on a core at `now`, made runnable at `made` (0: not known). */
static inline void switch_busy_dispatched(struct switch_busy *b, uint64_t now, uint64_t made)
{
    const uint64_t ready = made != 0 && made >= b->off && made <= now ? made : now;

    if (!b->seen) {
        return;
    }
    if (b->off_idle) {
        __builtin_memset(&b->e, 0, sizeof(b->e));
        b->open = true;
        b->e.woke = ready;
        b->cpu0 = b->off_cpu;
    } else if (b->open && b->off_waiting) {
        struct switch_busy_wait w = b->pending;

        w.ticks = ready - b->off;
        b->e.blocked += w.ticks;
        b->e.waits++;
        if (w.hint < SWITCH_BUSY_HINTS) {
            b->e.hint_ticks[w.hint] += w.ticks;
            b->e.hint_waits[w.hint]++;
        }
        switch_busy_keep_longest(&b->e, &w);
    } else if (b->open) {
        b->e.preemptions++;
    }
    if (b->open) {
        b->e.runnable += now - ready;
    }
    b->off_waiting = b->off_idle = false;
}

static inline uint64_t switch_busy_us(uint64_t ticks, uint32_t numer, uint32_t denom)
{
    return ticks * numer / denom / 1000u;
}

static inline size_t switch_busy_put(size_t size, size_t at, int n)
{
    if (n < 0 || at >= size) {
        return at;
    }
    return at + (size_t)n < size ? at + (size_t)n : size - 1;
}

static inline void switch_busy_where(char *out, size_t size, uint8_t where, uint64_t value)
{
    if (where == SWITCH_STALL_DSC) {
        snprintf(out, size, "dsc 0x%llx", (unsigned long long)value);
    } else if (where == SWITCH_STALL_MAIN) {
        snprintf(out, size, "main+0x%llx", (unsigned long long)value);
    } else {
        snprintf(out, size, "0x%llx", (unsigned long long)value);
    }
}

/* block_hint.h's name for a hint, or its number past the ones stall.h names (the page waits among them). */
static inline void switch_busy_kind(char *out, size_t size, unsigned int hint)
{
    if (hint < SWITCH_STALL_WAIT_NAMES) {
        snprintf(out, size, "%s", switch_stall_wait_name(hint));
    } else {
        snprintf(out, size, "wait kind 0x%x", hint);
    }
}

/*
 * A finished stretch, after the caller's head (thread and length): on a core, blocked by kind (largest first, at most
 * `kinds` of them), waiting for a core, then the longest waits. Times in the timebase numer/denom. Returns the length
 * written, at most size - 1.
 */
static inline size_t switch_busy_format(char *out, size_t size, const struct switch_busy_episode *e, uint32_t numer,
                                        uint32_t denom, unsigned int kinds)
{
    bool used[SWITCH_BUSY_HINTS] = {false};
    char kind[40];
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    at = switch_busy_put(size, at,
                         snprintf(out, size, "on core %llu us, blocked %llu us in %u waits (",
                                  (unsigned long long)switch_busy_us(e->cpu, numer, denom),
                                  (unsigned long long)switch_busy_us(e->blocked, numer, denom), e->waits));
    for (unsigned int k = 0; k < kinds; k++) {
        unsigned int best = SWITCH_BUSY_HINTS;

        for (unsigned int h = 0; h < SWITCH_BUSY_HINTS; h++) {
            if (!used[h] && e->hint_waits[h] != 0 &&
                (best == SWITCH_BUSY_HINTS || e->hint_ticks[h] > e->hint_ticks[best])) {
                best = h;
            }
        }
        if (best == SWITCH_BUSY_HINTS) {
            break;
        }
        used[best] = true;
        switch_busy_kind(kind, sizeof(kind), best);
        at = switch_busy_put(size, at,
                             snprintf(out + at, size - at, "%s%s %llu us/%u", k ? ", " : "", kind,
                                      (unsigned long long)switch_busy_us(e->hint_ticks[best], numer, denom),
                                      e->hint_waits[best]));
    }
    at = switch_busy_put(size, at,
                         snprintf(out + at, size - at, "), waiting for a core %llu us, %u preemptions; longest waits:",
                                  (unsigned long long)switch_busy_us(e->runnable, numer, denom), e->preemptions));
    for (unsigned int i = 0; i < SWITCH_BUSY_LONGEST && e->longest[i].ticks != 0; i++) {
        const struct switch_busy_wait *w = &e->longest[i];
        char pc[32], lr[32];

        switch_busy_where(pc, sizeof(pc), w->pc_where, w->pc);
        switch_busy_where(lr, sizeof(lr), w->lr_where, w->lr);
        switch_busy_kind(kind, sizeof(kind), w->hint);
        at =
            switch_busy_put(size, at,
                            snprintf(out + at, size - at, " %llu us %s, user pc %s lr %s, kernel 0x%llx, event 0x%llx;",
                                     (unsigned long long)switch_busy_us(w->ticks, numer, denom), kind, pc, lr,
                                     (unsigned long long)w->kernel, (unsigned long long)w->event));
    }
    return at;
}

#endif /* SWITCH_SCHED_BUSY_H */
