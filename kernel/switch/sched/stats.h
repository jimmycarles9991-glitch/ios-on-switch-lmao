// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch scheduling stats (stats.c, debug kernels): how long the threads a frame depends on wait between
 * being made runnable and running, and the class the scheduler gave them. This half is plain data and text, free of
 * kernel types, so kernel/tests runs it on the host; the including file declares snprintf.
 */
#ifndef SWITCH_SCHED_STATS_H
#define SWITCH_SCHED_STATS_H

#include <stddef.h>
#include <stdint.h>

/* Latency histogram, log2 microseconds: bucket 0 is under 1 us, bucket b in [2^(b-1), 2^b) us, the last unbounded. */
#define SWITCH_SCHED_BUCKETS 24

struct switch_sched_window {
    uint32_t hist[SWITCH_SCHED_BUCKETS];
    uint32_t runs;
    uint32_t failsafe_runs; /* runs while demoted by the fixed/realtime fail-safe (TH_SFLAG_FAILSAFE) */
    uint32_t limit_blocks;  /* sleeps imposed by a CPU limit (proc_setthread_cpupercent's ledger block) */
    uint64_t limit_us;      /* their length, to the ledger's refill */
    uint64_t max_us;
};

/* The class of a thread as of its last run (sched.h's TH_MODE_*, thread QoS, clutch bucket). */
struct switch_sched_class {
    uint8_t base_pri;
    uint8_t sched_pri;
    uint8_t mode;   /* sched_mode_t: 1 realtime, 2 fixed, 3 timeshare */
    uint8_t qos;    /* THREAD_QOS_*, 0 unspecified */
    uint8_t bucket; /* TH_BUCKET_*: 0 fixpri, 1 FG, 2 IN, 3 DF, 4 UT, 5 BG */
};

static inline unsigned int switch_sched_bucket(uint64_t us)
{
    unsigned int b = us == 0 ? 0 : 64 - (unsigned int)__builtin_clzll(us);

    return b < SWITCH_SCHED_BUCKETS ? b : SWITCH_SCHED_BUCKETS - 1;
}

static inline void switch_sched_note(struct switch_sched_window *w, uint64_t us)
{
    w->hist[switch_sched_bucket(us)]++;
    w->runs++;
    if (us > w->max_us) {
        w->max_us = us;
    }
}

/* The bound under which pct percent of the runs waited: its bucket's upper edge, never above the longest wait. */
static inline uint64_t switch_sched_percentile(const struct switch_sched_window *w, unsigned int pct)
{
    uint64_t need = ((uint64_t)w->runs * pct + 99) / 100;
    uint64_t seen = 0;

    if (w->runs == 0) {
        return 0;
    }
    for (unsigned int b = 0; b < SWITCH_SCHED_BUCKETS; b++) {
        seen += w->hist[b];
        if (seen >= need) {
            uint64_t edge = b + 1 < SWITCH_SCHED_BUCKETS ? 1ULL << b : w->max_us;
            return edge < w->max_us ? edge : w->max_us;
        }
    }
    return w->max_us;
}

static inline const char *switch_sched_mode_name(uint8_t mode)
{
    static const char *const names[] = {"--", "RT", "FX", "TS"};
    return mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "??";
}

static inline const char *switch_sched_qos_name(uint8_t qos)
{
    static const char *const names[] = {"-", "MT", "BG", "UT", "DF", "IN", "UI"};
    return qos < sizeof(names) / sizeof(names[0]) ? names[qos] : "?";
}

static inline const char *switch_sched_bucket_name(uint8_t bucket)
{
    static const char *const names[] = {"FIX", "FG", "IN", "DF", "UT", "BG"};
    return bucket < sizeof(names) / sizeof(names[0]) ? names[bucket] : "?";
}

/* The end of `out` after an snprintf at `at` returned n: clamped to the buffer, unchanged on an error. */
static inline size_t switch_sched_appended(size_t at, int n, size_t size)
{
    if (n < 0) {
        return at;
    }
    return at + (size_t)n < size ? at + (size_t)n : size - 1;
}

/*
 * One thread's entry: "label[pid] base/sched mode qos bucket: runs p50/p99/max us", then the fail-safe and CPU-limit
 * counts when there are any. Returns the length written (snprintf's, clamped to the buffer).
 */
static inline size_t switch_sched_format(char *out, size_t size, const char *label, int pid,
                                         const struct switch_sched_class *c, const struct switch_sched_window *w)
{
    size_t at = 0;
    int n = 0;

    if (size == 0) {
        return 0;
    }
    n = snprintf(out, size, "%s[%d] %u/%u %s %s %s: %u runs %llu/%llu/%llu us", label, pid, c->base_pri, c->sched_pri,
                 switch_sched_mode_name(c->mode), switch_sched_qos_name(c->qos), switch_sched_bucket_name(c->bucket),
                 w->runs, (unsigned long long)switch_sched_percentile(w, 50),
                 (unsigned long long)switch_sched_percentile(w, 99), (unsigned long long)w->max_us);
    at = switch_sched_appended(0, n, size);
    if (w->failsafe_runs != 0 && at < size) {
        n = snprintf(out + at, size - at, ", %u fail-safe", w->failsafe_runs);
        at = switch_sched_appended(at, n, size);
    }
    if (w->limit_blocks != 0 && at < size) {
        n = snprintf(out + at, size - at, ", CPU limit %u/%llu us", w->limit_blocks, (unsigned long long)w->limit_us);
        at = switch_sched_appended(at, n, size);
    }
    return at;
}

#endif /* SWITCH_SCHED_STATS_H */
