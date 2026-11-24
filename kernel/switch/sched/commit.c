// SPDX-License-Identifier: GPL-2.0-only
/*
 * The commit path (commit.h), debug kernels: a client's commit timed against backboardd's two Core Animation threads.
 * The render-server thread's own dispatches and departures write the server's side, the display thread's dispatches
 * (render.c) the shown side; the render server publishes to the display thread through a two-entry buffer. The newest
 * commit's send time is one word every committer stores (frames.h switch_frames_last_commit reads it). The 10 s
 * scheduling report adds the "SwitchSched commit path" line.
 */

#include <kern/misc_protos.h>
#include <switch/frames.h>
#include <switch/sched.h>

#include "commit.h"

#include <kern/block_hint.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <os/log.h>

#if DEVELOPMENT || DEBUG

static uint64_t last_commit; /* the newest commit message's time, every committer's store */
static uint64_t server_tid;
static struct switch_commit_server server;
static struct switch_commit_shown shown;
static struct switch_commit_shown shown_reported;

static uint64_t abs_us(uint64_t ticks)
{
    uint64_t ns = 0;

    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / NSEC_PER_USEC;
}

void switch_sched_commit_sent(uint64_t now)
{
    os_atomic_max(&last_commit, now, relaxed);
}

/* thread_dispatch or thread_invoke, under the thread's lock: true when it is the render-server thread (its side
 * written). No lock, allocation or log here. */
bool switch_sched_commit_server(thread_t thread, uint64_t now, bool departing)
{
    if (thread->thread_id != os_atomic_load(&server_tid, relaxed)) {
        return false;
    }
    if (departing) {
        const unsigned int hint = thread->block_hint;
        const bool waiting = (thread->state & TH_WAIT) != 0;
        const bool receiving = waiting && (hint == kThreadWaitPortReceive || hint == kThreadWaitPortSetReceive);
        const bool port_set = receiving && !waitq_is_null(thread->waitq) && waitq_type(thread->waitq) == WQT_PORT_SET;

        if (switch_commit_departed(&server, os_atomic_load(&last_commit, relaxed), receiving, port_set, now)) {
            os_atomic_store(&server.published, server.published + 1, release);
        }
    } else {
        const uint64_t made = thread->last_made_runnable_time;

        switch_commit_dispatched(&server, os_atomic_load(&last_commit, relaxed), made == THREAD_NOT_RUNNABLE ? 0 : made,
                                 now);
    }
    return true;
}

/* render.c: the display thread is dispatched at `now`, under its lock. */
void switch_sched_watch_display_woke(uint64_t now)
{
    const uint64_t published = os_atomic_load(&server.published, acquire);
    const struct switch_commit_applied a = server.applied[published % 2];

    os_atomic_thread_fence(acquire);
    switch_commit_woke(&shown, &a, published, os_atomic_load(&server.published, relaxed), now, abs_us);
}

void switch_sched_commit_server_named(uint64_t tid)
{
    os_atomic_store(&server_tid, tid, relaxed);
}

void switch_sched_commit_report(char *line, size_t size, uint64_t window_ns)
{
    const struct switch_commit_shown now_shown = shown;
    size_t at = (size_t)snprintf(line, size, "SwitchSched commit path (%llu ms): ", window_ns / NSEC_PER_MSEC);

    at = MIN(at, size - 1);
    switch_commit_format(line + at, size - at, &now_shown, &shown_reported);
    shown_reported = now_shown;
    os_log(OS_LOG_DEFAULT, "%s", line);
}

uint64_t switch_frames_last_commit(void)
{
    return os_atomic_load(&last_commit, relaxed);
}

#else /* DEVELOPMENT || DEBUG */

uint64_t switch_frames_last_commit(void)
{
    return 0;
}

#endif /* DEVELOPMENT || DEBUG */
