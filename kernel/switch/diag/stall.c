// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch UI stall reports (debug kernels): one os_log line when the main thread of SpringBoard, backboardd, chronod or
 * a WidgetKit extension has been off core for SWITCH_STALL_NS, blocked on anything but its idle run loop or runnable
 * and not run, and one when it runs again. Each watched thread costs one sample under its lock per tick, a stall one
 * snapshot and two lines; a widget extension's exec costs one lookup of its executable's path.
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
 * thread's own state. A "chain:" line follows when that owner waits for another in turn (up to STALL_MORE more) or a
 * wait on the way is a kernel rwlock, whose writer it names (stall_chain.c).
 *
 * The line that ends it names the threads that ran the most on the cores since its first line: the first line takes
 * every thread's time on a core, the end the gains since (the newest stall's: one snapshot).
 */

#include <switch/recorder.h>
#include <switch/sched.h>
#include <switch/xnu.h>

#include "stall.h"
#include "stall_snapshot.h"

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/processor.h>
#include <kern/recount.h>
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

#if defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED

void switch_stall_report(thread_call_param_t p0, thread_call_param_t p1);

#define STALL_CHECK_NS 250000000ull /* the most often the tick samples */
#define STALL_USED     4096         /* more threads than the system runs */
#define STALL_TOP      4
#define STALL_WIDGETS  4 /* the extensions exec'd last; a new one takes the slot of its own name, else the oldest */

extern void qsort(void *a, size_t n, size_t es, int (*cmp)(const void *, const void *));

struct stall_slot {
    const char *name; /* a widget slot's names its last exec, rewritten under stall_lock */
    bool widget;
    thread_t thread; /* +1: the main thread of the last exec of `name` */
    struct switch_stall_episode episode;
    struct stall_snapshot snap; /* taken when the episode began; its references held while report is set */
    bool report;                /* a begin line is due */
    bool ended;                 /* an end line is due */
    uint64_t ended_ns;          /* that episode's length, to the sample that saw it end */
};

static char stall_widget_names[STALL_WIDGETS][32];

static struct stall_slot stall_slots[] = {
    {.name = "SpringBoard"},
    {.name = "backboardd"},
    {.name = "chronod"},
    {.name = stall_widget_names[0], .widget = true},
    {.name = stall_widget_names[1], .widget = true},
    {.name = stall_widget_names[2], .widget = true},
    {.name = stall_widget_names[3], .widget = true},
};
static unsigned int stall_widget_next;

/* bsd/kern/kern_proc.c and bsd/vfs: the process's executable vnode (an iocount) and its path. */
extern void *proc_getexecutablevnode(void *p);
extern int vn_getpath(void *vp, char *pathbuf, int *len);
extern int vnode_put(void *vp);

static SIMPLE_LOCK_DECLARE(stall_lock, 0);
static thread_call_t stall_call;
static uint64_t stall_threshold, stall_check, stall_last;

/* The report's own (a ONCE thread call: never two at a time): the snapshot of the newest stall's first line, and whose */
static struct switch_stall_used stall_used[STALL_USED];
static unsigned int stall_used_n;
static bool stall_used_complete;
static int stall_used_slot = -1;

/* A WidgetKit extension, as switch_boot_band.c's band_widget_extension tells them: an .appex named like a widget. */
static bool stall_widget(task_t task, const char *name)
{
    char path[512];
    int len = sizeof(path);
    void *vp = NULL;
    bool widget = false;

    if (strnstr(name, "Widget", strlen(name)) == NULL && strcmp(name, "NewsToday2") != 0 &&
        strcmp(name, "NewsTag") != 0) {
        return false;
    }
    void *p = get_bsdtask_info(task);
    if (p == NULL) {
        return false;
    }
    vp = proc_getexecutablevnode(p);
    if (vp == NULL) {
        return false;
    }
    widget = vn_getpath(vp, path, &len) == 0 && strnstr(path, ".appex/", strlen(path)) != NULL;
    vnode_put(vp);
    return widget;
}

/* Under stall_lock: the slot a new process takes, or NULL. A widget extension takes the one of its name, else the
 * oldest, renamed. */
static struct stall_slot *stall_slot_for(const char *name, bool widget)
{
    const unsigned int n = sizeof(stall_slots) / sizeof(stall_slots[0]);
    unsigned int k = 0;

    for (unsigned int i = 0; i < n; i++) {
        if (stall_slots[i].widget == widget && strcmp(name, stall_slots[i].name) == 0) {
            return &stall_slots[i];
        }
    }
    if (!widget) {
        return NULL;
    }
    k = stall_widget_next++ % STALL_WIDGETS;
    strlcpy(stall_widget_names[k], name, sizeof(stall_widget_names[k]));
    return &stall_slots[n - STALL_WIDGETS + k];
}

/* kern_exec.c, after a successful posix_spawn or execve: the new main thread, if its process is a UI one. */
static void stall_exec_slot(thread_t thread)
{
    char name[32];
    struct stall_slot *slot = NULL;
    thread_t old = THREAD_NULL;
    struct stall_snapshot stale = {0};
    bool widget = false;

    switch_task_name(get_threadtask(thread), name, sizeof(name));
    widget = stall_widget(get_threadtask(thread), name);
    if (!widget && strcmp(name, "SpringBoard") != 0 && strcmp(name, "backboardd") != 0 &&
        strcmp(name, "chronod") != 0) {
        return;
    }
    if (os_atomic_load(&stall_call, acquire) == NULL) {
        thread_call_t call = thread_call_allocate_with_options(switch_stall_report, NULL, THREAD_CALL_PRIORITY_USER,
                                                               THREAD_CALL_OPTIONS_ONCE);

        nanoseconds_to_absolutetime(SWITCH_STALL_NS, &stall_threshold);
        nanoseconds_to_absolutetime(STALL_CHECK_NS, &stall_check);
        if (call != NULL && !os_atomic_cmpxchg(&stall_call, NULL, call, release)) {
            thread_call_free(call);
        }
    }
    thread_reference(thread);
    simple_lock(&stall_lock, LCK_GRP_NULL);
    slot = stall_slot_for(name, widget);
    if (slot != NULL) {
        old = slot->thread;
        if (slot->report) {
            stale = slot->snap;
        }
        slot->thread = thread;
        slot->episode = (struct switch_stall_episode){0};
        slot->report = slot->ended = false;
    }
    simple_unlock(&stall_lock);
    if (slot == NULL) {
        thread_deallocate(thread);
        return;
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
static void stall_tick_slots(uint64_t now)
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
                switch_stall_take_rwlock(t, &slot->snap.wait);
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
            switch_stall_take_owner_chain(&slot->snap, STALL_MORE);
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
    struct stall_line l = {.at = 0}, chain = {.at = 0};
    const int pid = task_pid(ss->task);
    uint64_t off_ns = 0, since_ns = 0;
    bool chained;

    absolutetime_to_nanoseconds(ss->taken - episode.since, &off_ns);
    absolutetime_to_nanoseconds(episode.since, &since_ns);
    switch_stall_put(&l, "%s[%d] main thread 0x%llx %s %llu.%llu s (since %llu.%llu s of uptime, pri %d):", name, pid,
                     ss->tid, episode.kind == SWITCH_STALL_RUNNABLE ? "runnable, not run, for" : "blocked",
                     off_ns / 1000000000ull, off_ns / 100000000ull % 10, since_ns / 1000000000ull,
                     since_ns / 100000000ull % 10, ss->wait.sched_pri);
    switch_stall_put_wait(&l, ss);
    switch_stall_put_kernel(&l, "kernel", &ss->wait);
    switch_stall_put_user(&l, ss);
    if (episode.kind == SWITCH_STALL_RUNNABLE) {
        struct stall_cpu cpus[STALL_CPUS];

        switch_stall_census(cpus);
        switch_stall_put_census(&l, cpus);
    }
    chained = switch_stall_put_chain(&chain, ss);
    switch_stall_release(ss);
    os_log(OS_LOG_DEFAULT, "SwitchStall: %s", l.text);
    if (chained) {
        os_log(OS_LOG_DEFAULT, "SwitchStall: %s[%d] main thread 0x%llx chain:%s", name, pid, ss->tid, chain.text);
    }
}

static void stall_used_take(int slot)
{
    thread_t t = THREAD_NULL;
    unsigned int n = 0;
    bool complete = true;

    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&threads, t, thread_t, threads)
    {
        if (n == STALL_USED) {
            complete = false;
            break;
        }
        stall_used[n].tid = thread_tid(t);
        stall_used[n].cpu = recount_thread_time_mach(t);
        n++;
    }
    lck_mtx_unlock(&tasks_threads_lock);
    qsort(stall_used, n, sizeof(stall_used[0]), switch_stall_used_cmp);
    stall_used_n = n;
    stall_used_complete = complete;
    stall_used_slot = slot;
}

/* "; on a core since its first line: <process>[pid] 0x<tid> <thread> <mode> <pri> <N> ms; ..." */
static void stall_put_used(struct stall_line *l)
{
    struct switch_stall_top top[STALL_TOP] = {0};
    thread_t t = THREAD_NULL;

    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&threads, t, thread_t, threads)
    {
        const uint64_t tid = thread_tid(t);
        const struct switch_stall_top e = {
            .tid = tid,
            .cpu = switch_stall_gained(stall_used, stall_used_n, stall_used_complete, tid, recount_thread_time_mach(t)),
            .ref = t,
        };

        switch_stall_rank(top, STALL_TOP, e);
    }
    for (unsigned int i = 0; i < STALL_TOP && top[i].ref != NULL; i++) {
        thread_reference((thread_t)top[i].ref);
    }
    lck_mtx_unlock(&tasks_threads_lock);
    if (top[0].ref != NULL) {
        switch_stall_put(l, "; on a core since its first line:");
    }
    for (unsigned int i = 0; i < STALL_TOP && top[i].ref != NULL; i++) {
        thread_t th = (thread_t)top[i].ref;
        char process[STALL_NAME];
        char name[64]; /* thread_get_thread_name copies MAXTHREADNAMESIZE bytes */
        uint64_t ns = 0;

        switch_task_name(get_threadtask(th), process, sizeof(process));
        thread_get_thread_name(th, name);
        absolutetime_to_nanoseconds(top[i].cpu, &ns);
        switch_stall_put(l, " %s[%d] 0x%llx %s %s %d %llu ms;", process, task_pid(get_threadtask(th)), top[i].tid,
                         name[0] ? name : "-", switch_stall_mode_name(th->sched_mode), th->sched_pri, ns / 1000000ull);
        thread_deallocate(th);
    }
}

void switch_stall_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    for (unsigned int i = 0; i < sizeof(stall_slots) / sizeof(stall_slots[0]); i++) {
        struct stall_slot *slot = &stall_slots[i];
        struct switch_stall_episode episode;
        struct stall_snapshot snap;
        char name[32];
        thread_t t;
        bool report, ended;
        uint64_t ended_ns;

        simple_lock(&stall_lock, LCK_GRP_NULL);
        strlcpy(name, slot->name, sizeof(name));
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
            stall_report_one(name, &snap, episode);
            stall_used_take((int)i);
        }
        if (ended) {
            struct stall_line l = {.at = 0};

            switch_stall_put(&l, "%s[%d] main thread 0x%llx ran again %llu.%llu s after it %s", name,
                             task_pid(get_threadtask(t)), thread_tid(t), ended_ns / 1000000000ull,
                             ended_ns / 100000000ull % 10,
                             episode.kind == SWITCH_STALL_RUNNABLE ? "was made runnable" : "blocked");
            if (stall_used_slot == (int)i) {
                stall_put_used(&l);
                stall_used_slot = -1;
            }
            os_log(OS_LOG_DEFAULT, "SwitchStall: %s", l.text);
        }
        thread_deallocate(t);
    }
}

#endif /* SWITCH_STATS_SCHED */

void switch_stall_exec(thread_t thread)
{
    switch_sched_busy_exec(thread);
#if SWITCH_RECORDER
    switch_recorder_exec(thread);
#endif
#if defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED
    stall_exec_slot(thread);
#endif
}

void switch_stall_tick(uint64_t now)
{
    switch_sched_busy_tick();
#if SWITCH_RECORDER
    switch_recorder_tick(now);
#endif
#if defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED
    stall_tick_slots(now);
#endif
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
