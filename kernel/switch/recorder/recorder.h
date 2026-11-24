// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder's decisions and its status line (recorder.c, watch.c), free of kernel types so kernel/tests runs
 * them on the host; the including file declares snprintf. Times are mach ticks.
 */
#ifndef SWITCH_RECORDER_H
#define SWITCH_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum switch_recorder_reason {
    SWITCH_RECORDER_NONE = 0,
    SWITCH_RECORDER_LATE_FRAME, /* the display: a present six periods or more after the one before */
    SWITCH_RECORDER_HANG,       /* a UI main thread's busy stretch (sched/busy.c) reached SWITCH_RECORDER_HANG_NS */
    SWITCH_RECORDER_SLOW_WORK, /* user space: a state creation or an import over its bound (debug.switch_recorder_slow) */
    SWITCH_RECORDER_MARK,      /* the left Joy-Con's Capture, a Bluetooth controller's, or debug.switch_recorder_mark */
    SWITCH_RECORDER_REASONS
};

static inline const char *switch_recorder_reason_name(uint32_t reason)
{
    static const char *const names[SWITCH_RECORDER_REASONS] = {"none", "late_frame", "hang", "slow_work", "mark"};

    return names[reason < SWITCH_RECORDER_REASONS ? reason : 0];
}

#define SWITCH_RECORDER_HANG_NS   250000000ull   /* HangTracer's bound for a hang */
#define SWITCH_RECORDER_GAP_NS    10000000000ull /* the least time between two dumps: see recorder.c on their cost */
#define SWITCH_RECORDER_TAIL_NS   300000000ull   /* how long after a frame or a mark the ring is frozen */
#define SWITCH_RECORDER_EXPECT_NS 5000000000ull  /* how long debug.switch_recorder_expect holds other triggers */
#define SWITCH_RECORDER_SLOTS     8u             /* dump files kept, each overwritten in turn */
#define SWITCH_RECORDER_SLOW_MS   100u /* the least creation or import time debug.switch_recorder_slow takes */

/*
 * The dump's state: idle, claimed by a trigger writing its thread and value, queued for a reason, or running. A trigger
 * takes an idle recorder; a hang also takes one queued for another reason (a late frame's or a mark's tail), since the
 * hung thread's stackshot cannot wait.
 */
#define SWITCH_RECORDER_IDLE    0u
#define SWITCH_RECORDER_CLAIM   0x80u
#define SWITCH_RECORDER_RUNNING 0x100u

static inline bool switch_recorder_takes(uint32_t state, uint32_t reason)
{
    if (state == SWITCH_RECORDER_IDLE) {
        return true;
    }
    return reason == SWITCH_RECORDER_HANG && state < SWITCH_RECORDER_REASONS && state != SWITCH_RECORDER_HANG;
}

static inline bool switch_recorder_in_gap(uint64_t last, uint64_t now, uint64_t gap)
{
    return last != 0 && now - last < gap;
}

/*
 * A UI main thread's hang: its busy stretch (sched/busy.c's, from its departures and dispatches) open since `woke` for
 * the bound. One dump per stretch, latched only once a trigger took it, so a stretch a dump in flight or the gap turned
 * away fires again at the next tick it still lasts.
 */
struct switch_recorder_watch {
    uint64_t fired_woke; /* the stretch a trigger took, 0: none */
};

static inline bool switch_recorder_watch_due(const struct switch_recorder_watch *w, bool open, uint64_t woke,
                                             uint64_t now, uint64_t bound)
{
    return open && woke != 0 && now > woke && now - woke >= bound && woke != w->fired_woke;
}

static inline void switch_recorder_watch_fired(struct switch_recorder_watch *w, uint64_t woke)
{
    w->fired_woke = woke;
}

/* Dumps, each into the next of the slots. */
struct switch_recorder_gate {
    uint32_t dumps;
    unsigned slot; /* the next dump's */
};

static inline void switch_recorder_gate_done(struct switch_recorder_gate *g)
{
    g->dumps++;
    g->slot = (g->slot + 1) % SWITCH_RECORDER_SLOTS;
}

struct switch_recorder_last {
    uint32_t reason;
    uint64_t tid, value;
    unsigned slot;
    uint64_t trace_bytes, shot_bytes;
    uint64_t trace_us, shot_us; /* the dump's own cost: the ring is off while it is written */
};

/* The recorder's own steady cost, read at the status line. */
struct switch_recorder_cost {
    uint32_t ring;    /* events the ring holds */
    uint64_t span_ms; /* from its oldest event to now, once it has wrapped; 0 before */
    uint64_t samples; /* kperf timer samples taken since boot, all CPUs */
};

static inline uint64_t switch_recorder_events_s(const struct switch_recorder_cost *c)
{
    return c->span_ms ? (uint64_t)c->ring * 1000 / c->span_ms : 0;
}

struct switch_recorder_counts {
    uint32_t triggers[SWITCH_RECORDER_REASONS];
    uint32_t skipped; /* inside the gap */
    uint32_t held;    /* another thread's while debug.switch_recorder_expect held the recorder */
    uint32_t taken;   /* the ring was another process's */
};

/* debug.switch_recorder: key=value fields HW Test reads (KernelLine.fields). */
static inline int switch_recorder_status(char *out, size_t size, const struct switch_recorder_cost *c,
                                         const struct switch_recorder_counts *n, const struct switch_recorder_gate *g,
                                         const struct switch_recorder_last *last, uint64_t age_ms)
{
    return snprintf(out, size,
                    "ring=%u span_ms=%llu events_s=%llu samples=%llu dumps=%u skipped=%u held=%u taken=%u "
                    "late_frame=%u hang=%u slow_work=%u mark=%u last_reason=%s last_tid=%llu last_value=%llu "
                    "last_slot=%u last_trace_bytes=%llu last_shot_bytes=%llu last_trace_us=%llu last_shot_us=%llu "
                    "last_age_ms=%llu",
                    c->ring, (unsigned long long)c->span_ms, (unsigned long long)switch_recorder_events_s(c),
                    (unsigned long long)c->samples, g->dumps, n->skipped, n->held, n->taken,
                    n->triggers[SWITCH_RECORDER_LATE_FRAME], n->triggers[SWITCH_RECORDER_HANG],
                    n->triggers[SWITCH_RECORDER_SLOW_WORK], n->triggers[SWITCH_RECORDER_MARK],
                    switch_recorder_reason_name(last->reason), (unsigned long long)last->tid,
                    (unsigned long long)last->value, last->slot, (unsigned long long)last->trace_bytes,
                    (unsigned long long)last->shot_bytes, (unsigned long long)last->trace_us,
                    (unsigned long long)last->shot_us, (unsigned long long)age_ms);
}

#endif
