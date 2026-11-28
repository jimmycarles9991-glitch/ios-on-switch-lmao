#include "xnu_sched_stats_harness.h"

#include <stdio.h>

#include "switch_sched_stats.h"

static struct switch_sched_window window;

unsigned xnu_sched_buckets(void)
{
    return SWITCH_SCHED_BUCKETS;
}

unsigned xnu_sched_bucket(uint64_t us)
{
    return switch_sched_bucket(us);
}

void xnu_sched_reset(void)
{
    window = (struct switch_sched_window){0};
}

void xnu_sched_note(uint64_t us)
{
    switch_sched_note(&window, us);
}

void xnu_sched_take_longest(uint64_t us, uint64_t began_ms, uint64_t start_ms)
{
    (void)switch_sched_take_longest(&window, switch_sched_longest(us, began_ms), start_ms, 0);
}

/* One slot as stats.c keeps it, through the same helpers: the dispatches' word, and each take's window start. */
static struct switch_sched_window timeline_window;
static uint64_t timeline_word, timeline_start_ms;

void xnu_sched_timeline_reset(void)
{
    timeline_window = (struct switch_sched_window){0};
    timeline_word = timeline_start_ms = 0;
}

void xnu_sched_timeline_dispatch(uint64_t made_us, uint64_t now_us)
{
    const uint64_t us = now_us > made_us ? now_us - made_us : 0;
    const uint64_t word = switch_sched_longest_since(us, made_us);

    switch_sched_note(&timeline_window, us);
    if (word > timeline_word) {
        timeline_word = word;
    }
}

size_t xnu_sched_timeline_take(uint64_t now_us, char *out, size_t size)
{
    const struct switch_sched_class backboardd = {.base_pri = 53, .sched_pri = 53, .mode = 3, .qos = 5, .bucket = 1};
    size_t n = 0;

    timeline_start_ms = switch_sched_take_longest(&timeline_window, timeline_word, timeline_start_ms, now_us);
    timeline_word = 0;
    n = switch_sched_format(out, size, "backboardd.main", 69, &backboardd, &timeline_window);
    timeline_window = (struct switch_sched_window){0};
    return n;
}

uint64_t xnu_sched_longest(uint64_t us, uint64_t began_ms)
{
    return switch_sched_longest(us, began_ms);
}

uint64_t xnu_sched_before_ms(void)
{
    return window.max_before_ms;
}

void xnu_sched_set_counts(uint32_t failsafe_runs, uint32_t limit_blocks, uint64_t limit_us)
{
    window.failsafe_runs = failsafe_runs;
    window.limit_blocks = limit_blocks;
    window.limit_us = limit_us;
}

uint64_t xnu_sched_percentile(unsigned pct)
{
    return switch_sched_percentile(&window, pct);
}

uint64_t xnu_sched_max(void)
{
    return window.max_us;
}

uint32_t xnu_sched_runs(void)
{
    return window.runs;
}

size_t xnu_sched_format(char *out, size_t size, const char *label, int pid, uint8_t base_pri, uint8_t sched_pri,
                        uint8_t mode, uint8_t qos, uint8_t bucket)
{
    const struct switch_sched_class c = {
        .base_pri = base_pri,
        .sched_pri = sched_pri,
        .mode = mode,
        .qos = qos,
        .bucket = bucket,
    };
    return switch_sched_format(out, size, label, pid, &c, &window);
}
