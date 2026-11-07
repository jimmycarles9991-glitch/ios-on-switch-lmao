// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch UI stall reports (debug kernels): one os_log line when SpringBoard's or backboardd's main thread has been off
 * core for SWITCH_STALL_NS, blocked on anything but its idle run loop or runnable and not run, and one when it runs
 * again.
 *
 * Detection rides the scheduler's maintenance tick (sched_timeshare_maintenance_continue, woken from context switches
 * and quantum expiry at most every sched_tick_interval): no timer of its own, no wakeup of an idle system. The tick
 * samples the two threads under their thread locks and, when a stall begins, takes the snapshot there
 * (stall_snapshot.h); the line is written on a thread call because it reads the stalled thread's user stack (copyinmap,
 * which can fault), names processes (proc_name, which takes proc_list_lock) and, for a thread runnable and not run,
 * asks the other cores what they run.
 *
 * The line names the wait: block hint, wait queue type and object, wait event, the kernel continuation or the first
 * kernel return addresses (unslid: `atos -o kernel.macho`), the user PC, LR and frame chain (dsc <unslid> for the
 * shared cache, main+<offset>), and who it waits for: the process that received the request a special reply port
 * waits on (or the receiver of the port it is still queued at), the receiver of the port a send is blocked on, or the
 * thread or workqueue at the end of its turnstile's inheritor chain (lock holders, sync IPC servers), with that
 * thread's own state.
 */

#include <switch/xnu.h>

#include "stall.h"
#include "stall_snapshot.h"

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/simple_lock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/waitq.h>
#include <os/log.h>
#include <string.h>

void switch_stall_exec(thread_t thread);
void switch_stall_tick(uint64_t now);

#if DEVELOPMENT || DEBUG

void switch_stall_report(thread_call_param_t p0, thread_call_param_t p1);

#define STALL_CHECK_NS 250000000ull /* the most often the tick samples */

struct stall_slot {
    const char *name;
    thread_t thread; /* +1: the main thread of the last exec of `name` */
    struct switch_stall_episode episode;
    struct stall_snapshot snap; /* taken when the episode began; its references held while report is set */
    bool report;                /* a begin line is due */
    bool ended;                 /* an end line is due */
    uint64_t ended_ns;          /* that episode's length, to the sample that saw it end */
};

static struct stall_slot stall_slots[] = {
    {.name = "SpringBoard"},
    {.name = "backboardd"},
};

static SIMPLE_LOCK_DECLARE(stall_lock, 0);
static thread_call_t stall_call;
static uint64_t stall_threshold, stall_check, stall_last;

/* kern_exec.c, after a successful posix_spawn or execve: the new main thread, if its process is a UI one. */
void switch_stall_exec(thread_t thread)
{
    char name[32];
    thread_t old = THREAD_NULL;
    struct stall_snapshot stale = {0};

    switch_task_name(get_threadtask(thread), name, sizeof(name));
    for (unsigned int i = 0; i < sizeof(stall_slots) / sizeof(stall_slots[0]); i++) {
        struct stall_slot *slot = &stall_slots[i];

        if (strcmp(name, slot->name) != 0) {
            continue;
        }
        if (os_atomic_load(&stall_call, acquire) == NULL) {
            thread_call_t call = thread_call_allocate_with_options(
                switch_stall_report, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);

            nanoseconds_to_absolutetime(SWITCH_STALL_NS, &stall_threshold);
            nanoseconds_to_absolutetime(STALL_CHECK_NS, &stall_check);
            if (call != NULL && !os_atomic_cmpxchg(&stall_call, NULL, call, release)) {
                thread_call_free(call);
            }
        }
        thread_reference(thread);
        simple_lock(&stall_lock, LCK_GRP_NULL);
        old = slot->thread;
        if (slot->report) {
            stale = slot->snap;
        }
        slot->thread = thread;
        slot->episode = (struct switch_stall_episode){0};
        slot->report = slot->ended = false;
        simple_unlock(&stall_lock);
        break;
    }
    switch_stall_release(&stale);
    if (old != THREAD_NULL) {
        thread_deallocate(old);
    }
}

static void stall_sample(thread_t t, uint64_t now, struct switch_stall_sample *s)
{
    const int state = t->state;
    const bool queued = thread_get_runq(t) != PROCESSOR_NULL;

    s->waiting = (state & (TH_WAIT | TH_RUN)) == TH_WAIT;
    s->runnable = (state & (TH_WAIT | TH_RUN)) == TH_RUN && queued;
    s->running = (state & TH_RUN) && !queued;
    /* The kernel's hint for any receive is kThreadWaitPortReceive; a port set's wait queue is the run loop's. */
    s->run_loop_idle = !waitq_is_null(t->waitq) && waitq_type(t->waitq) == WQT_PORT_SET;
    s->now = now;
    s->last_run = t->last_run_time;
    s->made_runnable = t->last_made_runnable_time;
    s->threshold = stall_threshold;
}

/* sched_prim.c sched_timeshare_maintenance_continue, on the scheduler's maintenance thread. */
void switch_stall_tick(uint64_t now)
{
    bool due = false;

    if (os_atomic_load(&stall_call, acquire) == NULL || now - stall_last < stall_check) {
        return;
    }
    stall_last = now;
    simple_lock(&stall_lock, LCK_GRP_NULL);
    for (unsigned int i = 0; i < sizeof(stall_slots) / sizeof(stall_slots[0]); i++) {
        struct stall_slot *slot = &stall_slots[i];
        struct switch_stall_sample sample;
        thread_t t = slot->thread;
        bool began = false;
        spl_t s;

        if (t == THREAD_NULL) {
            continue;
        }
        s = splsched();
        thread_lock(t);
        if (t->state & TH_TERMINATE) {
            thread_unlock(t);
            splx(s);
            continue;
        }
        stall_sample(t, now, &sample);
        switch (switch_stall_step(&slot->episode, &sample)) {
        case SWITCH_STALL_BEGAN:
            if (!slot->report) {
                slot->snap = (struct stall_snapshot){0};
                switch_stall_take(t, &slot->snap);
                slot->report = due = began = true;
            }
            break;
        case SWITCH_STALL_ENDED:
            absolutetime_to_nanoseconds(now - slot->episode.since, &slot->ended_ns);
            slot->ended = due = true;
            break;
        case SWITCH_STALL_QUIET:
        default:
            break;
        }
        thread_unlock(t);
        splx(s);
        if (began) {
            switch_stall_take_owner(&slot->snap);
        }
    }
    simple_unlock(&stall_lock);
    if (due) {
        thread_call_enter(stall_call);
    }
}

/* Releases the snapshot's references. */
static void stall_report_one(const char *name, struct stall_snapshot *ss, struct switch_stall_episode episode)
{
    struct stall_line l = {.at = 0};
    uint64_t off_ns = 0, since_ns = 0;

    absolutetime_to_nanoseconds(ss->taken - episode.since, &off_ns);
    absolutetime_to_nanoseconds(episode.since, &since_ns);
    switch_stall_put(
        &l, "%s[%d] main thread 0x%llx %s %llu.%llu s (since %llu.%llu s of uptime, pri %d):", name, task_pid(ss->task),
        ss->tid, episode.kind == SWITCH_STALL_RUNNABLE ? "runnable, not run, for" : "blocked", off_ns / 1000000000ull,
        off_ns / 100000000ull % 10, since_ns / 1000000000ull, since_ns / 100000000ull % 10, ss->wait.sched_pri);
    switch_stall_put_wait(&l, ss);
    switch_stall_put_kernel(&l, "kernel", &ss->wait);
    switch_stall_put_user(&l, ss);
    if (episode.kind == SWITCH_STALL_RUNNABLE) {
        struct stall_cpu cpus[STALL_CPUS];

        switch_stall_census(cpus);
        switch_stall_put_census(&l, cpus);
    }
    switch_stall_release(ss);
    os_log(OS_LOG_DEFAULT, "SwitchStall: %s", l.text);
}

void switch_stall_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    for (unsigned int i = 0; i < sizeof(stall_slots) / sizeof(stall_slots[0]); i++) {
        struct stall_slot *slot = &stall_slots[i];
        struct switch_stall_episode episode;
        struct stall_snapshot snap;
        thread_t t;
        bool report, ended;
        uint64_t ended_ns;

        simple_lock(&stall_lock, LCK_GRP_NULL);
        t = slot->thread;
        report = slot->report;
        ended = slot->ended;
        ended_ns = slot->ended_ns;
        episode = slot->episode;
        snap = slot->snap;
        slot->report = slot->ended = false;
        if (t != THREAD_NULL && (report || ended)) {
            thread_reference(t);
        }
        simple_unlock(&stall_lock);
        if (t == THREAD_NULL || !(report || ended)) {
            continue;
        }
        if (report) {
            stall_report_one(slot->name, &snap, episode);
        }
        if (ended) {
            os_log(OS_LOG_DEFAULT, "SwitchStall: %s[%d] main thread 0x%llx ran again %llu.%llu s after it %s",
                   slot->name, task_pid(get_threadtask(t)), thread_tid(t), ended_ns / 1000000000ull,
                   ended_ns / 100000000ull % 10,
                   episode.kind == SWITCH_STALL_RUNNABLE ? "was made runnable" : "blocked");
        }
        thread_deallocate(t);
    }
}

/*
 * The frame timeline (NativeLegacyDisplay's "SwitchFrame" line) names the scheduling of the thread that submitted a
 * swap: base and current priority, mode (1 realtime, 2 fixed, 3 timeshare) and effective QoS. Unlocked reads: a
 * snapshot for a log line.
 */
void switch_thread_sched(thread_t t, int *base_pri, int *sched_pri, int *mode, int *qos);

void switch_thread_sched(thread_t t, int *base_pri, int *sched_pri, int *mode, int *qos)
{
    *base_pri = t->base_pri;
    *sched_pri = t->sched_pri;
    *mode = t->sched_mode;
    *qos = t->effective_policy.thep_qos;
}

#else /* DEVELOPMENT || DEBUG */

void switch_stall_exec(__unused thread_t thread)
{}

void switch_stall_tick(__unused uint64_t now)
{}

#endif /* DEVELOPMENT || DEBUG */
