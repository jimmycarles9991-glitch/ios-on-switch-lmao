// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thread call functions by the CPU they used, per thread call group (callouts.c, debug kernels). The scheduling report
 * names a thread call worker ("thread call kernel-high #3") but not the functions it ran; thread_call_invoke adds each
 * function's on-core time and calls to its CPU's table under the call's group, and the report adds the tables up and
 * names each group's busiest functions. IOTimerEventSource's thread call timeouts add the driver action they ran, with
 * no group. A table has one writer, its CPU with interrupts masked, and is read by the report; a function a full table
 * has no row for goes to its `other` row. Rows are never freed: a kernel has a bounded set of thread call functions.
 * Plain C, so kernel/tests runs it on the host; the including file declares snprintf. Times are mach ticks.
 */
#ifndef SWITCH_SCHED_CALLOUTS_H
#define SWITCH_SCHED_CALLOUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_CALLOUT_ROWS      64u /* a power of two */
#define SWITCH_CALLOUT_SUM       256u
#define SWITCH_CALLOUT_GROUPS    16u
#define SWITCH_CALLOUT_PER_GROUP 2u

struct switch_callout_row {
    uint64_t func;  /* 0: empty */
    uint64_t group; /* the thread call group's name (a static string), 0 for a timer action */
    uint64_t cpu, calls;
};

struct switch_callout_table {
    struct switch_callout_row rows[SWITCH_CALLOUT_ROWS];
    struct switch_callout_row other;
};

static inline void switch_callout_bump(uint64_t *field, uint64_t n)
{
    __atomic_store_n(field, __atomic_load_n(field, __ATOMIC_RELAXED) + n, __ATOMIC_RELAXED);
}

static inline void switch_callout_add(struct switch_callout_table *t, uint64_t func, uint64_t group, uint64_t cpu)
{
    uint32_t i = (uint32_t)(((func ^ group) * 0x9e3779b97f4a7c15ull) >> 58) & (SWITCH_CALLOUT_ROWS - 1);
    struct switch_callout_row *row = &t->other;

    for (uint32_t probe = 0; probe < SWITCH_CALLOUT_ROWS; probe++, i = (i + 1) & (SWITCH_CALLOUT_ROWS - 1)) {
        const uint64_t at = __atomic_load_n(&t->rows[i].func, __ATOMIC_RELAXED);

        if (at == func && t->rows[i].group == group) {
            row = &t->rows[i];
            break;
        }
        if (at == 0) {
            row = &t->rows[i];
            row->group = group;
            __atomic_store_n(&row->func, func, __ATOMIC_RELEASE);
            break;
        }
    }
    switch_callout_bump(&row->cpu, cpu);
    switch_callout_bump(&row->calls, 1);
}

/* The report's sums by function and group across every CPU's table: running totals, or one window's share. */
struct switch_callout_sum {
    uint32_t count;
    struct switch_callout_row rows[SWITCH_CALLOUT_SUM];
    struct switch_callout_row other;
};

static inline struct switch_callout_row *switch_callout_sum_row(struct switch_callout_sum *s, uint64_t func,
                                                                uint64_t group)
{
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->rows[i].func == func && s->rows[i].group == group) {
            return &s->rows[i];
        }
    }
    if (s->count == SWITCH_CALLOUT_SUM) {
        return &s->other;
    }
    struct switch_callout_row *row = &s->rows[s->count++];
    row->func = func;
    row->group = group;
    row->cpu = row->calls = 0;
    return row;
}

static inline void switch_callout_sum_table(struct switch_callout_sum *s, const struct switch_callout_table *t)
{
    for (uint32_t i = 0; i < SWITCH_CALLOUT_ROWS; i++) {
        const uint64_t func = __atomic_load_n(&t->rows[i].func, __ATOMIC_ACQUIRE);

        if (func != 0) {
            struct switch_callout_row *row = switch_callout_sum_row(s, func, t->rows[i].group);

            row->cpu += __atomic_load_n(&t->rows[i].cpu, __ATOMIC_RELAXED);
            row->calls += __atomic_load_n(&t->rows[i].calls, __ATOMIC_RELAXED);
        }
    }
    s->other.cpu += __atomic_load_n(&t->other.cpu, __ATOMIC_RELAXED);
    s->other.calls += __atomic_load_n(&t->other.calls, __ATOMIC_RELAXED);
}

static inline uint64_t switch_callout_less(uint64_t now, uint64_t was)
{
    return now > was ? now - was : 0;
}

/* `window` gets what each row added since `seen`, which becomes `now`. */
static inline void switch_callout_window(const struct switch_callout_sum *now, struct switch_callout_sum *seen,
                                         struct switch_callout_sum *window)
{
    window->count = 0;
    for (uint32_t i = 0; i < now->count; i++) {
        const struct switch_callout_row *r = &now->rows[i];
        struct switch_callout_row *was = switch_callout_sum_row(seen, r->func, r->group);
        struct switch_callout_row *w = switch_callout_sum_row(window, r->func, r->group);

        w->cpu += switch_callout_less(r->cpu, was->cpu);
        w->calls += switch_callout_less(r->calls, was->calls);
        was->cpu = r->cpu;
        was->calls = r->calls;
    }
    window->other.cpu = switch_callout_less(now->other.cpu, seen->other.cpu);
    window->other.calls = switch_callout_less(now->other.calls, seen->other.calls);
    seen->other = now->other;
}

static inline size_t switch_callout_put(size_t size, size_t at, int n)
{
    if (n < 0 || at >= size) {
        return at;
    }
    return at + (size_t)n < size ? at + (size_t)n : size - 1;
}

struct switch_callout_group {
    uint64_t group, cpu;
};

/* The window's groups, busiest first; returns how many. */
static inline uint32_t switch_callout_groups(const struct switch_callout_sum *window, struct switch_callout_group *g)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < window->count; i++) {
        uint32_t k = 0;

        while (k < n && g[k].group != window->rows[i].group) {
            k++;
        }
        if (k == n) {
            if (n == SWITCH_CALLOUT_GROUPS) {
                continue;
            }
            g[n].group = window->rows[i].group;
            g[n++].cpu = 0;
        }
        g[k].cpu += window->rows[i].cpu;
    }
    for (uint32_t i = 1; i < n; i++) {
        const struct switch_callout_group t = g[i];
        uint32_t j = i;

        for (; j > 0 && g[j - 1].cpu < t.cpu; j--) {
            g[j] = g[j - 1];
        }
        g[j] = t;
    }
    return n;
}

/* The busiest row of `group` not yet shown, or SWITCH_CALLOUT_SUM. */
static inline uint32_t switch_callout_busiest(const struct switch_callout_sum *window, uint64_t group,
                                              const bool *shown)
{
    uint32_t best = SWITCH_CALLOUT_SUM;

    for (uint32_t i = 0; i < window->count; i++) {
        const struct switch_callout_row *r = &window->rows[i];

        if (!shown[i] && r->group == group && r->cpu != 0 &&
            (best == SWITCH_CALLOUT_SUM || r->cpu > window->rows[best].cpu)) {
            best = i;
        }
    }
    return best;
}

static inline unsigned long long switch_callout_ms(uint64_t ticks, uint32_t numer, uint32_t denom)
{
    return (unsigned long long)(ticks * numer / denom / 1000000u);
}

/* One group's "kernel-high 0xfffffff0081a2c40 736 ms (5400), ... (others 3 ms)": its busiest functions. */
static inline size_t switch_callout_format_group(char *out, size_t size, const struct switch_callout_sum *window,
                                                 const struct switch_callout_group *g, const char *name,
                                                 uint64_t (*unslide)(uint64_t), uint32_t numer, uint32_t denom)
{
    bool shown[SWITCH_CALLOUT_SUM] = {false};
    uint64_t listed = 0;
    size_t at = switch_callout_put(size, 0, snprintf(out, size, "%s", name));

    for (uint32_t n = 0; n < SWITCH_CALLOUT_PER_GROUP; n++) {
        const uint32_t best = switch_callout_busiest(window, g->group, shown);

        if (best == SWITCH_CALLOUT_SUM) {
            break;
        }
        shown[best] = true;
        listed += window->rows[best].cpu;
        at = switch_callout_put(size, at,
                                snprintf(out + at, size - at, "%s 0x%llx %llu ms (%llu)", n ? "," : "",
                                         (unsigned long long)unslide(window->rows[best].func),
                                         switch_callout_ms(window->rows[best].cpu, numer, denom),
                                         (unsigned long long)window->rows[best].calls));
    }
    return switch_callout_put(
        size, at, snprintf(out + at, size - at, " (others %llu ms)", switch_callout_ms(g->cpu - listed, numer, denom)));
}

/*
 * "kernel 0x... 736 ms (5400), 0x... 12 ms (40) (others 3 ms); kernel-high ...; timer actions ...; unlisted 0 ms (0)":
 * every group with CPU in the window, busiest first. `name` gives a group's name.
 */
static inline size_t switch_callout_format(char *out, size_t size, const struct switch_callout_sum *window,
                                           const char *(*name)(uint64_t), uint64_t (*unslide)(uint64_t), uint32_t numer,
                                           uint32_t denom)
{
    struct switch_callout_group g[SWITCH_CALLOUT_GROUPS];
    const uint32_t n = switch_callout_groups(window, g);
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    for (uint32_t k = 0; k < n && g[k].cpu != 0; k++) {
        if (k != 0) {
            at = switch_callout_put(size, at, snprintf(out + at, size - at, "; "));
        }
        at += switch_callout_format_group(out + at, size - at, window, &g[k], name(g[k].group), unslide, numer, denom);
    }
    return switch_callout_put(size, at,
                              snprintf(out + at, size - at, "%sunlisted %llu ms (%llu)", at ? "; " : "",
                                       switch_callout_ms(window->other.cpu, numer, denom),
                                       (unsigned long long)window->other.calls));
}

#endif /* SWITCH_SCHED_CALLOUTS_H */
