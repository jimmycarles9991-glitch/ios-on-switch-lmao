// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch frame stalls (debug kernels): when the display finds the screen stopped in the middle of an animation (no
 * present for a few refresh periods after presents every frame), one os_log line per thread a frame depends on, as it
 * is at that frame end: Core Animation's render-server thread in backboardd, SpringBoard's main thread and the focal
 * app's. Each says whether the thread is on a core, on a run queue (with what the other cores run) or blocked, and
 * then on what and behind whom (stall_snapshot.h).
 *
 * The threads are those sched/stats.c learns: backboardd naming its render-server thread, RunningBoard giving a task
 * the focal role (SpringBoard's main thread, the frontmost app's) and taking it away. The snapshot is taken on the
 * display's work loop at the frame end; the lines are written on a thread call.
 */

#include <switch/stall.h>
#include <switch/xnu.h>

#include "stall_snapshot.h"

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/simple_lock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <mach/task_policy.h>
#include <os/log.h>
#include <string.h>

#if DEVELOPMENT || DEBUG

enum frame_thread {
    FRAME_RENDER_SERVER,
    FRAME_SPRINGBOARD,
    FRAME_APP,
    FRAME_THREADS,
};

static const char *const frame_labels[FRAME_THREADS] = {"render-server", "main", "main"};

struct frame_report {
    uint64_t last_present;
    thread_t thread[FRAME_THREADS]; /* +1 */
    struct stall_snapshot snap[FRAME_THREADS];
    bool on_core[FRAME_THREADS];
    uint64_t last_run[FRAME_THREADS], made_runnable[FRAME_THREADS];
};

static SIMPLE_LOCK_DECLARE(frame_lock, 0);
static thread_t frame_watched[FRAME_THREADS]; /* +1 each */
static thread_call_t frame_call;
static bool frame_busy; /* a report taken and not yet written */
static struct frame_report frame_pending;

static void frame_write(thread_call_param_t p0, thread_call_param_t p1);

/* Consumes the reference on thread (THREAD_NULL: stop watching). */
static void frame_watch(enum frame_thread which, thread_t thread)
{
    thread_t old;

    if (os_atomic_load(&frame_call, acquire) == NULL) {
        thread_call_t call =
            thread_call_allocate_with_options(frame_write, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);

        if (call != NULL && !os_atomic_cmpxchg(&frame_call, NULL, call, release)) {
            thread_call_free(call);
        }
    }
    simple_lock(&frame_lock, LCK_GRP_NULL);
    old = frame_watched[which];
    frame_watched[which] = thread;
    simple_unlock(&frame_lock);
    if (old != THREAD_NULL) {
        thread_deallocate(old);
    }
}

static thread_t main_thread(task_t task)
{
    thread_t thread = THREAD_NULL;

    task_lock(task);
    if (!queue_empty(&task->threads)) {
        thread = (thread_t)queue_first(&task->threads);
        thread_reference(thread);
    }
    task_unlock(task);
    return thread;
}

void switch_stall_frame_role(task_t task, int role)
{
    char name[32];
    thread_t thread, gone = THREAD_NULL;

    if (role == TASK_FOREGROUND_APPLICATION) {
        if ((thread = main_thread(task)) == THREAD_NULL) {
            return;
        }
        switch_task_name(task, name, sizeof(name));
        frame_watch(strcmp(name, "SpringBoard") == 0 ? FRAME_SPRINGBOARD : FRAME_APP, thread);
        return;
    }
    simple_lock(&frame_lock, LCK_GRP_NULL);
    if (frame_watched[FRAME_APP] != THREAD_NULL && get_threadtask(frame_watched[FRAME_APP]) == task) {
        gone = frame_watched[FRAME_APP];
        frame_watched[FRAME_APP] = THREAD_NULL;
    }
    simple_unlock(&frame_lock);
    if (gone != THREAD_NULL) {
        thread_deallocate(gone);
    }
}

void switch_stall_frame_render_server(thread_t thread)
{
    thread_reference(thread);
    frame_watch(FRAME_RENDER_SERVER, thread);
}

/* The display's work loop, at a frame end. */
void switch_stall_frame(uint64_t last_present)
{
    struct frame_report *r = &frame_pending;
    thread_call_t call = os_atomic_load(&frame_call, acquire);

    if (call == NULL) {
        return;
    }
    simple_lock(&frame_lock, LCK_GRP_NULL);
    if (frame_busy) {
        simple_unlock(&frame_lock);
        return;
    }
    frame_busy = true;
    *r = (struct frame_report){.last_present = last_present};
    for (unsigned int i = 0; i < FRAME_THREADS; i++) {
        if ((r->thread[i] = frame_watched[i]) != THREAD_NULL) {
            thread_reference(r->thread[i]);
        }
    }
    simple_unlock(&frame_lock);
    for (unsigned int i = 0; i < FRAME_THREADS; i++) {
        thread_t t = r->thread[i];
        spl_t s;

        if (t == THREAD_NULL) {
            continue;
        }
        s = splsched();
        thread_lock(t);
        if (!(t->state & TH_TERMINATE)) {
            r->on_core[i] = switch_stall_on_core(t);
            r->last_run[i] = t->last_run_time;
            r->made_runnable[i] = t->last_made_runnable_time;
            switch_stall_take(t, &r->snap[i]);
        }
        thread_unlock(t);
        splx(s);
        switch_stall_take_owner(&r->snap[i]);
    }
    thread_call_enter(call);
}

static uint64_t ms_between(uint64_t from, uint64_t to)
{
    uint64_t ns = 0;

    if (to > from) {
        absolutetime_to_nanoseconds(to - from, &ns);
    }
    return ns / NSEC_PER_MSEC;
}

static void frame_write_one(const struct frame_report *r, unsigned int i, const struct stall_cpu *cpus)
{
    const struct stall_snapshot *ss = &r->snap[i];
    struct stall_line l = {.at = 0};
    char process[32];
    bool runnable = false;

    switch_task_name(ss->task, process, sizeof(process));
    switch_stall_put(&l, "frame: no present for %llu ms: %s[%d] %s thread 0x%llx",
                     ms_between(r->last_present, ss->taken), process, task_pid(ss->task), frame_labels[i], ss->tid);
    if (r->on_core[i]) {
        switch_stall_put(&l, " on core (pri %d)", ss->wait.sched_pri);
    } else if (switch_stall_off_core_waiting(ss->wait.state)) {
        switch_stall_put(&l, " blocked %llu ms (pri %d):", ms_between(r->last_run[i], ss->taken), ss->wait.sched_pri);
        switch_stall_put_wait(&l, ss);
        switch_stall_put_kernel(&l, "kernel", &ss->wait);
    } else {
        switch_stall_put(&l, " runnable, not run, for %llu ms (pri %d):", ms_between(r->made_runnable[i], ss->taken),
                         ss->wait.sched_pri);
        switch_stall_put_wait(&l, ss);
        runnable = true;
    }
    switch_stall_put_user(&l, ss);
    if (runnable && cpus != NULL) {
        switch_stall_put_census(&l, cpus);
    }
    os_log(OS_LOG_DEFAULT, "SwitchStall %s", l.text);
}

static void frame_write(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    struct frame_report *r = &frame_pending;
    struct stall_cpu cpus[STALL_CPUS];
    bool census = false;

    for (unsigned int i = 0; i < FRAME_THREADS; i++) {
        const struct stall_snapshot *ss = &r->snap[i];

        if (ss->task != TASK_NULL && !r->on_core[i] && !switch_stall_off_core_waiting(ss->wait.state)) {
            census = true;
        }
    }
    if (census) {
        switch_stall_census(cpus);
    }
    for (unsigned int i = 0; i < FRAME_THREADS; i++) {
        if (r->snap[i].task != TASK_NULL) {
            frame_write_one(r, i, census ? cpus : NULL);
            switch_stall_release(&r->snap[i]);
        }
        if (r->thread[i] != THREAD_NULL) {
            thread_deallocate(r->thread[i]);
        }
    }
    simple_lock(&frame_lock, LCK_GRP_NULL);
    frame_busy = false;
    simple_unlock(&frame_lock);
}

#else /* DEVELOPMENT || DEBUG */

void switch_stall_frame(__unused uint64_t last_present)
{}

#endif /* DEVELOPMENT || DEBUG */
