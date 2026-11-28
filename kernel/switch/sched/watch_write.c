// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watched threads (watch.h), debug kernels: the lines. The writer, on a thread call, drains the slots' rings into
 * "SwitchCommit:", "SwitchTouchSend:" and "SwitchBusy: ... (the focal app)" lines and logs "SwitchLaunch:"; the 10 s
 * scheduling report adds the "SwitchSched watch" line and frees the slots of threads that are gone: a thread no longer
 * on the system's thread list has had its last dispatch, so no hook writes its slot.
 */

#include <kern/misc_protos.h>
#include <switch/sched.h>

#include "commit.h"
#include "watch_slot.h"

#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <os/log.h>
#include <string.h>

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED

#define WATCH_LINES 20 /* a second */

static uint32_t writer_dropped, writer_dropped_reported;

static long long ms_before(uint64_t then, uint64_t now)
{
    uint64_t ns = 0;

    absolutetime_to_nanoseconds(now > then ? now - then : 0, &ns);
    return (long long)(ns / NSEC_PER_MSEC);
}

/* The focal app's first commit since it became focal, both times as milliseconds before this line. */
static void write_launch(char *line, size_t size)
{
    struct switch_watch_launch_snap snap;
    uint64_t now = 0;

    if (!switch_watch_launch_take(&switch_watch_launch, &snap)) {
        return;
    }
    now = mach_absolute_time();
    (void)snprintf(line, size,
                   "SwitchLaunch: %s[%d] became the focal app %lld ms before this line; its first commit %lld ms "
                   "before it (%lld ms after it became focal)",
                   snap.name, snap.pid, ms_before(snap.focal_at, now), ms_before(snap.first_commit, now),
                   ms_before(snap.focal_at, snap.first_commit));
    os_log(OS_LOG_DEFAULT, "%s", line);
}

static void write_one(const struct switch_watch_event *ev, char *line, size_t size, const mach_timebase_info_data_t *tb)
{
    int n = 0;

    if (ev->kind == SWITCH_WATCH_BUSY) {
        uint64_t woke_ns = 0;

        absolutetime_to_nanoseconds(ev->e.woke, &woke_ns);
        n = snprintf(line, size,
                     "SwitchBusy: %s[%d] main thread 0x%llx (the focal app) busy %llu ms from its run loop's wake at "
                     "%llu.%03llu s: ",
                     ev->name, ev->pid, ev->tid,
                     (unsigned long long)(switch_busy_us(ev->e.end - ev->e.woke, tb->numer, tb->denom) / 1000),
                     woke_ns / NSEC_PER_SEC, woke_ns % NSEC_PER_SEC / NSEC_PER_MSEC);
        if (n > 0 && (size_t)n < size) {
            switch_busy_format(line + n, size - (size_t)n, &ev->e, tb->numer, tb->denom, 4);
        }
        os_log(OS_LOG_DEFAULT, "%s", line);
        return;
    }
    if (ev->kind == SWITCH_WATCH_TOUCH) {
        const uint64_t began = switch_busy_us(
            ev->e.woke > ev->report ? ev->e.woke - ev->report : ev->report - ev->e.woke, tb->numer, tb->denom);

        n = snprintf(line, size,
                     "SwitchTouchSend: %s[%d] thread 0x%llx %u/%u %s %s to pid %d: report to send %llu us, the "
                     "stretch began %s%llu us after the report; ",
                     ev->name, ev->pid, ev->tid, ev->base_pri, ev->sched_pri, switch_sched_mode_name(ev->mode),
                     switch_sched_qos_name(ev->qos), ev->to_pid,
                     (unsigned long long)switch_busy_us(ev->e.end - ev->report, tb->numer, tb->denom),
                     ev->e.woke < ev->report ? "-" : "", (unsigned long long)began);
    } else {
        n = snprintf(line, size, "SwitchCommit: %s[%d] thread 0x%llx %u/%u %s %s: ", ev->name, ev->pid, ev->tid,
                     ev->base_pri, ev->sched_pri, switch_sched_mode_name(ev->mode), switch_sched_qos_name(ev->qos));
    }
    if (n > 0 && (size_t)n < size) {
        switch_watch_event_format(line + n, size - (size_t)n, ev, tb->numer, tb->denom);
    }
    os_log(OS_LOG_DEFAULT, "%s", line);
}

/* The writer's thread call (watch.c's tick enters it). At most WATCH_LINES lines a second; the rest are counted. */
void switch_watch_write(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    static char line[900]; /* under the 1 KiB os_log record */
    static uint64_t second_start;
    static uint32_t second_lines;
    mach_timebase_info_data_t tb;
    uint64_t second = 0;

    clock_timebase_info(&tb);
    nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
    write_launch(line, sizeof(line));
    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        struct watch_slot *s = &switch_watch_slots[i];
        uint32_t tail = os_atomic_load(&s->tail, relaxed);

        while (tail != os_atomic_load(&s->head, acquire)) {
            const struct switch_watch_event ev = s->ring[tail % WATCH_RING];
            const uint64_t now = mach_absolute_time();

            os_atomic_store(&s->tail, ++tail, release);
            if (now - second_start > second) {
                second_start = now;
                second_lines = 0;
            }
            if (second_lines++ >= WATCH_LINES) {
                writer_dropped++;
                continue;
            }
            write_one(&ev, line, sizeof(line), &tb);
        }
    }
}

/* stats.c's 10 s report, after its own lines: the window's events and the commit path. */
void switch_sched_watch_report(char *line, size_t size, uint64_t window_ns)
{
    struct switch_watch_counts sum = {0};
    const uint32_t dropped = os_atomic_load(&writer_dropped, relaxed);
    uint32_t used = 0;
    size_t at = 0;

    if (os_atomic_load(&switch_sched_watch_on, relaxed) == 0) {
        return;
    }
    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        struct watch_slot *s = &switch_watch_slots[i];
        struct switch_watch_counts c;

        if (!switch_slot_taken(os_atomic_load(&switch_watch_table.tids[i], acquire))) {
            continue;
        }
        used++;
        c = s->counts;
        switch_watch_add(&sum, &c, &s->reported);
        s->reported = c;
    }
    sum.dropped += dropped - writer_dropped_reported;
    writer_dropped_reported = dropped;
    at = (size_t)snprintf(line, size,
                          "SwitchSched watch (%llu ms; events late past %u ms for a commit, %u ms "
                          "for a touch delivery): ",
                          window_ns / NSEC_PER_MSEC, COMMIT_LATE_US / 1000, TOUCH_LATE_US / 1000);
    at = MIN(at, size - 1);
    at += switch_watch_counts_format(line + at, size - at, &sum);
    (void)snprintf(line + at, size - at,
                   "; threads watched %u (%u taken, %u freed since boot; %u refused a slot, once a window each)", used,
                   os_atomic_load(&switch_watch_claimed, relaxed), os_atomic_load(&switch_watch_freed, relaxed),
                   os_atomic_xchg(&switch_watch_refused, 0, relaxed));
    os_log(OS_LOG_DEFAULT, "%s", line);
    switch_sched_commit_report(line, size, window_ns);
    switch_slot_prune_filter(&switch_watch_table);
}

#endif /* (DEVELOPMENT || DEBUG) && SWITCH_STATS_SCHED */
