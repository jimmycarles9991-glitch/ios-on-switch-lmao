// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watched threads (watch.h), debug kernels: the hooks. A thread is watched from its first commit to the render server,
 * its first touch delivery from backboardd, or, for the focal app's main thread, its first dispatch after RunningBoard
 * made the app focal: it takes a slot itself (slots.h), on a core or in its own dispatch, without a lock. From then
 * its own departures and dispatches (thread_dispatch and thread_invoke, under its lock) keep its stretches, and its
 * events close them; nothing else writes the slot. A late event and a focal main thread's stretch between two run-loop
 * waits of SWITCH_BUSY_MIN_MS or more go into the slot's ring, which the thread alone fills and the writer
 * (watch_write.c) alone drains; the scheduler's maintenance tick puts the writer on a thread call. A launch is the
 * focal app's first commit after it became focal.
 */

#include <kern/misc_protos.h>
#include <switch/sched.h>
#include <switch/xnu.h>

#include "commit.h"
#include "watch_slot.h"

#include <kern/block_hint.h>
#include <kern/clock.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <machine/machine_routines.h>
#include <string.h>

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED

_Static_assert(SWITCH_WATCH_HINT_PARKED == kThreadWaitParkedWorkQueue, "parked workqueue hint");
#if XNU_12377_121_6
_Static_assert(SWITCH_WATCH_HINT_PARKED_BOUND == kThreadWaitParkedBoundWorkQueue, "bound parked workqueue hint");
#endif

#define BURST_US 2000

uint32_t switch_sched_watch_on;
struct switch_slots switch_watch_table;
struct watch_slot switch_watch_slots[SWITCH_SLOTS];
struct switch_watch_launch switch_watch_launch;
uint32_t switch_watch_claimed, switch_watch_freed, switch_watch_refused;
static thread_call_t watch_call;
static uint64_t burst_abs, commit_late_abs, touch_late_abs, focal_busy_abs;
static uint64_t focal_tid; /* the focal app's main thread (stats.c's role hook writes, its threads' hooks read) */

static struct watch_slot *slot_of(uint64_t tid)
{
    const int i = switch_slot_find(&switch_watch_table, tid);

    return i < 0 ? NULL : &switch_watch_slots[i];
}

/* The thresholds, once; their readers find a slot first, whose publication (release) comes after this store. */
static void watch_times(void)
{
    uint64_t burst = 0;

    if (os_atomic_load(&burst_abs, acquire) != 0) {
        return;
    }
    nanoseconds_to_absolutetime(COMMIT_LATE_US * NSEC_PER_USEC, &commit_late_abs);
    nanoseconds_to_absolutetime(TOUCH_LATE_US * NSEC_PER_USEC, &touch_late_abs);
    nanoseconds_to_absolutetime(SWITCH_BUSY_MIN_MS * NSEC_PER_MSEC, &focal_busy_abs);
    nanoseconds_to_absolutetime(BURST_US * NSEC_PER_USEC, &burst);
    os_atomic_store(&burst_abs, burst, release);
}

/*
 * The thread `tid` of `task`, on a core or in its own dispatch: a slot, set up, then published. Only the stretches are
 * cleared for a new owner: the counts go on (the report takes differences) and the ring's head and tail stay monotonic,
 * so the writer, which never locks, never sees them go back; every ring entry carries its own owner's name, pid and
 * tid.
 */
static struct watch_slot *slot_take(thread_t thread, task_t task)
{
    const uint64_t tid = thread->thread_id;
    bool first = false;
    const int i = switch_slot_claim(&switch_watch_table, tid, &first);
    struct watch_slot *s = NULL;

    if (i < 0) {
        if (first) {
            os_atomic_inc(&switch_watch_refused, relaxed);
        }
        return slot_of(tid);
    }
    watch_times();
    s = &switch_watch_slots[i];
    memset(&s->t, 0, sizeof(s->t));
    memset(&s->run, 0, sizeof(s->run));
    s->pid = task_pid(task);
    switch_task_name(task, s->name, sizeof(s->name));
    os_atomic_or(&thread->switch_watch_mask, SWITCH_WATCH_WATCH, relaxed);
    switch_slot_publish(&switch_watch_table, (unsigned int)i, tid);
    os_atomic_inc(&switch_watch_claimed, relaxed);
    os_atomic_store(&switch_sched_watch_on, 1, relaxed);
    return s;
}

static bool focal(const thread_t thread)
{
    return thread->thread_id == os_atomic_load(&focal_tid, relaxed);
}

static bool ring_room(const struct watch_slot *s)
{
    return s->head - os_atomic_load(&s->tail, acquire) < WATCH_RING;
}

static void ring_push(struct watch_slot *s)
{
    os_atomic_store(&s->head, s->head + 1, release);
}

/* thread_dispatch, under the departing thread's lock: no lock, allocation or log here. */
void switch_sched_watch_departed(thread_t thread, uint64_t now)
{
    const bool waiting = (thread->state & TH_WAIT) != 0;
    struct switch_busy_wait wait = {0};
    struct watch_slot *s = NULL;
    uint64_t cpu = 0, pc = 0;
    bool port_set = false, idle = false, room = false;
    struct switch_busy_episode *done = NULL;

    if (switch_sched_commit_server(thread, now, true)) {
        return;
    }
    s = slot_of(thread->thread_id);
    if (s == NULL) {
        return;
    }
    port_set = waiting && !waitq_is_null(thread->waitq) && waitq_type(thread->waitq) == WQT_PORT_SET;
    idle = waiting && switch_watch_idle(port_set, thread->block_hint);
    if (waiting && !idle) {
        switch_sched_wait_of(thread, &wait);
    } else if (!waiting) {
        const uint8_t where = switch_sched_user_site(thread, false, &pc);

        switch_busy_sampled(&s->t.busy, where, pc);
        switch_busy_sampled(&s->run, where, pc);
    }
    cpu = recount_thread_time_mach(thread);
    switch_busy_departed(&s->t.busy, now, cpu, waiting, idle, &wait, UINT64_MAX, NULL);
    room = ring_room(s);
    if (room) {
        done = &s->ring[s->head % WATCH_RING].e;
    }
    if (!switch_busy_departed(&s->run, now, cpu, waiting, port_set, &wait, focal(thread) ? focal_busy_abs : UINT64_MAX,
                              done)) {
        return;
    }
    if (!room) {
        os_atomic_store(&s->counts.dropped, s->counts.dropped + 1, relaxed);
        return;
    }
    struct switch_watch_event *ev = &s->ring[s->head % WATCH_RING];

    ev->kind = SWITCH_WATCH_BUSY;
    ev->tid = thread->thread_id;
    ev->pid = s->pid;
    memcpy(ev->name, s->name, sizeof(ev->name));
    ev->name[sizeof(ev->name) - 1] = '\0';
    ring_push(s);
}

/* thread_invoke, under the incoming thread's lock: no lock, allocation or log here. */
void switch_sched_watch_dispatched(thread_t thread, uint64_t now)
{
    const uint64_t made = thread->last_made_runnable_time == THREAD_NOT_RUNNABLE ? 0 : thread->last_made_runnable_time;
    struct watch_slot *s = NULL;

    if (switch_sched_commit_server(thread, now, false)) {
        return;
    }
    s = slot_of(thread->thread_id);
    if (s == NULL && focal(thread)) {
        s = slot_take(thread, get_threadtask(thread));
    }
    if (s != NULL) {
        switch_busy_dispatched(&s->t.busy, now, made);
        switch_busy_dispatched(&s->run, now, made);
    }
}

/* stats.c, thread context: RunningBoard made `task` (not SpringBoard) the focal app, main thread `thread`. */
void switch_sched_watch_focal(task_t task, thread_t thread)
{
    char name[SWITCH_WATCH_NAME];

    switch_task_name(task, name, sizeof(name));
    os_atomic_or(&thread->switch_watch_mask, SWITCH_WATCH_WATCH, relaxed);
    if (switch_watch_launch_set(&switch_watch_launch, task_pid(task), name, mach_absolute_time())) {
        os_atomic_store(&focal_tid, thread_tid(thread), relaxed);
    }
    os_atomic_store(&switch_sched_watch_on, 1, relaxed);
}

/* stats.c, thread context: backboardd named its render-server thread. */
void switch_sched_watch_render_server(thread_t thread)
{
    if (os_atomic_load(&watch_call, acquire) == NULL) {
        thread_call_t call = thread_call_allocate_with_options(switch_watch_write, NULL, THREAD_CALL_PRIORITY_USER,
                                                               THREAD_CALL_OPTIONS_ONCE);

        if (call != NULL && !os_atomic_cmpxchg(&watch_call, NULL, call, release)) {
            thread_call_free(call);
        }
    }
    os_atomic_or(&thread->switch_watch_mask, SWITCH_WATCH_WATCH, relaxed);
    switch_sched_commit_server_named(thread_tid(thread));
    os_atomic_store(&switch_sched_watch_on, 1, relaxed);
}

void switch_sched_watch_exiting(thread_t thread)
{
    if ((os_atomic_load(&thread->switch_watch_mask, relaxed) & SWITCH_WATCH_WATCH) == 0) {
        return;
    }
    disable_preemption();
    os_atomic_and(&thread->switch_watch_mask, (uint8_t)~SWITCH_WATCH_WATCH, relaxed);
    if (switch_slot_free_tid(&switch_watch_table, thread->thread_id)) {
        os_atomic_inc(&switch_watch_freed, relaxed);
    }
    enable_preemption();
}

static uint64_t own_cpu(void)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const uint64_t cpu = recount_current_thread_time_mach();

    ml_set_interrupts_enabled(enabled);
    return cpu;
}

/* The current thread's event at `now`: its stretch closed, counted, and kept for a line when it was late. */
static void watch_event(struct watch_slot *s, unsigned int kind, uint64_t now, uint64_t burst, uint64_t report,
                        int to_pid)
{
    const thread_t self = current_thread();
    const bool room = ring_room(s);
    struct switch_watch_event *ev = room ? &s->ring[s->head % WATCH_RING] : NULL;
    struct switch_watch_summary sum = {0};
    bool late = false;

    if (switch_watch_message(&s->t, now, own_cpu(), burst, ev, &sum) != SWITCH_WATCH_TIMED) {
        return;
    }
    late = kind == SWITCH_WATCH_TOUCH ? report != 0 && now - report >= touch_late_abs : sum.length >= commit_late_abs;
    switch_watch_count(&s->counts, kind, late, sum.cause);
    if (!late) {
        return;
    }
    if (!room) {
        os_atomic_store(&s->counts.dropped, s->counts.dropped + 1, relaxed);
        return;
    }
    ev->kind = (uint8_t)kind;
    ev->base_pri = (uint8_t)self->base_pri;
    ev->sched_pri = (uint8_t)self->sched_pri;
    ev->mode = (uint8_t)self->sched_mode;
    ev->qos = (uint8_t)self->effective_policy.thep_qos;
    ev->pid = s->pid;
    ev->to_pid = to_pid;
    ev->tid = self->thread_id;
    ev->report = report;
    memcpy(ev->name, s->name, sizeof(ev->name));
    ev->name[sizeof(ev->name) - 1] = '\0';
    ring_push(s);
}

/* frames.c, under the destination port's lock: a client's commit message to the render server, from this thread. */
void switch_sched_watch_commit(uint64_t now)
{
    const thread_t self = current_thread();
    struct watch_slot *s = slot_of(self->thread_id);

    switch_sched_commit_sent(now);
    switch_watch_launch_commit(&switch_watch_launch, task_pid(current_task()), now);
    if (s == NULL) {
        s = slot_take(self, current_task());
    }
    if (s != NULL) {
        watch_event(s, SWITCH_WATCH_COMMIT, now, os_atomic_load(&burst_abs, relaxed), 0, 0);
    }
}

/* frames.c: this backboardd thread sends the touch report of `report` (mach) on to `to_pid`. */
void switch_sched_watch_delivered(uint64_t report, int to_pid, uint64_t now)
{
    const thread_t self = current_thread();
    struct watch_slot *s = slot_of(self->thread_id);

    if (s == NULL) {
        s = slot_take(self, current_task());
    }
    if (s != NULL) {
        watch_event(s, SWITCH_WATCH_TOUCH, now, 0, report, to_pid);
    }
}

/* The scheduler's maintenance tick (busy.c): the writer is woken only when there is something to write. */
void switch_sched_watch_tick(void)
{
    const thread_call_t call = os_atomic_load(&watch_call, acquire);
    const struct switch_watch_launch *l = &switch_watch_launch;

    if (call == NULL) {
        return;
    }
    if (os_atomic_load(&l->first_commit, relaxed) != 0 &&
        os_atomic_load(&l->logged_seq, relaxed) != os_atomic_load(&l->seq, relaxed)) {
        thread_call_enter(call);
        return;
    }
    for (unsigned int i = 0; i < SWITCH_SLOTS; i++) {
        const struct watch_slot *s = &switch_watch_slots[i];

        if (os_atomic_load(&s->head, acquire) != os_atomic_load(&s->tail, relaxed)) {
            thread_call_enter(call);
            return;
        }
    }
}

#endif /* DEVELOPMENT || DEBUG */
