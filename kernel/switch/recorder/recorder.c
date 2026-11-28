// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder's triggers and its sysctls (--//kernel:recorder; <switch/recorder.h>, state.h).
 *
 * kdebug_startup starts kdebug wrapping through SWITCH_RECORDER_RING_EVENTS with switch_recorder_filter, owned by the
 * kernel, and kperf_init_early starts kperf's timer sampling the running thread's kernel and user stacks into the same
 * ring (sample.c). A trigger records one tracepoint and asks for a capture: recorder.h's switch_recorder_admit takes
 * every owner's mark, and an automatic ask (a late frame, a hang, slow work) when no other waits and recent ones are far
 * enough apart. A taken ask arms the capture call for its due time, which wakes capture.c's thread; a refused one is
 * kept for that thread to log. The capture copies the ring and a stackshot into memory in tens of milliseconds, and
 * the store (store.c) compresses and writes them later from a low-priority call, so nothing slow happens at the moment
 * being measured. A ring another process took (a ktrace session) is never read: its captures keep their stackshot.
 */

#include <switch/recorder.h>

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kdebug.h>
#include <sys/kdebug_common.h>
#include <sys/kdebug_private.h>
#include <sys/ktrace.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/simple_lock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <os/log.h>
#include <string.h>

#include "state.h"

#if SWITCH_RECORDER

/*
 * Scheduler, interrupts, Mach traps and BSD system calls, VM faults, file system reads and disk I/O (page-ins with
 * their vnodes), shared-region events, turnstiles, kperf's samples, our display and GPU tracepoints, the signpost
 * classes Apple's processes may emit, and new threads, execs and thread names (DBG_TRACE's data and strings: they name
 * the ring's threads that ended before its stackshot). Left out: Mach IPC (MACH_IPC_KMSG_INFO takes the destination
 * port's lock on every send in every process) and the Metal plugin's DBG_APPS 'S' points (they would turn into system
 * calls on its render thread); the kernel's flip and job tracepoints carry the frame instead. QuartzCore's window-server subclass (0x31ca)
 * is in: 0x31ca02c8 is its presentation latency, logged where it changes (IOMFBDisplay::can_update_, finish_update,
 * set_presentation_latency), which decides how many frame ends ahead it stamps a swap.
 */
char switch_recorder_filter[] = "S0x0140,S0x0105,S0x010c,S0x0130,S0x01a8,S0x040c,S0x0301,S0x0302,C0x25,S0x0612,"
                                "S0x210a,C0x2b,C0x35,S0x0700,S0x0701,S0x31ca";
bool switch_recorder_own_filter;

extern struct kd_control kd_control_trace;
extern struct kd_buffer kd_buffer_trace;

static struct {
    thread_call_t call; /* the capture call, published with the times below (release); NULL until kperf is set up */
    uint64_t tail, expect_window, refusal_every;
    thread_t dumper; /* the capture thread inside switch_recorder_kdebug_read: only it compares equal to itself */
    bool owns_ring;  /* the ring is the one kdebug_startup started for the recorder, never taken since */
    bool taken_logged;
    uint32_t triggers[SWITCH_RECORDER_REASONS], refused[SWITCH_RECORDER_REFUSALS_KINDS], taken; /* atomic */
    uint64_t expect_tid, expect_until;
    /* Under the trigger lock. */
    struct switch_recorder_asks asks;
    struct switch_recorder_recent recent;
    uint64_t armed;
    uint32_t seq;
    struct switch_recorder_ask refusal[SWITCH_RECORDER_REFUSALS];
    uint8_t refusal_why[SWITCH_RECORDER_REFUSALS];
    uint32_t refusals;
    struct switch_recorder_refusal_seen seen;
} switch_recorder;

static SIMPLE_LOCK_DECLARE(switch_recorder_trigger_lock, 0);

char *switch_recorder_boot_filter(unsigned int events, char *filter)
{
    if (events != 0) {
        return filter;
    }
    switch_recorder.owns_ring = true;
    switch_recorder_own_filter = true;
    return switch_recorder_filter;
}

/* sample.c, once kperf is configured at boot: thread calls exist by then, and no trigger can come before it. */
void switch_recorder_start(void)
{
    thread_call_t call = NULL;

    if (!switch_recorder_store_init() || !switch_recorder_capture_init()) {
        os_log(OS_LOG_DEFAULT, "SwitchRecorder: off (no memory for the store or the capture thread)");
        return;
    }
    /* The call only wakes the capture thread: its group's priority runs it on time, the thread's keeps the capture
     * under the render server. */
    call = thread_call_allocate_with_options(switch_recorder_capture_call, NULL, THREAD_CALL_PRIORITY_HIGH,
                                             THREAD_CALL_OPTIONS_ONCE);
    nanoseconds_to_absolutetime(SWITCH_RECORDER_TAIL_NS, &switch_recorder.tail);
    nanoseconds_to_absolutetime(SWITCH_RECORDER_GAP_NS, &switch_recorder.recent.gap);
    nanoseconds_to_absolutetime(SWITCH_RECORDER_REPEAT_NS, &switch_recorder.recent.repeat);
    nanoseconds_to_absolutetime(SWITCH_RECORDER_EXPECT_NS, &switch_recorder.expect_window);
    nanoseconds_to_absolutetime(SWITCH_RECORDER_REFUSAL_NS, &switch_recorder.refusal_every);
    os_atomic_store(&switch_recorder.call, call, release);
}

bool switch_recorder_dumping(void)
{
    return os_atomic_load(&switch_recorder.dumper, relaxed) == current_thread();
}

void switch_recorder_dumping_set(bool on)
{
    os_atomic_store(&switch_recorder.dumper, on ? current_thread() : THREAD_NULL, relaxed);
}

bool switch_recorder_owns_ring(void)
{
    return os_atomic_load(&switch_recorder.owns_ring, relaxed);
}

/* capture.c, before it reads the ring: still the recorder's, tracing on and no ktrace session holding it. */
bool switch_recorder_ring_ours(void)
{
    int owner = 0;

    if (!switch_recorder_owns_ring()) {
        return false;
    }
    ktrace_lock();
    owner = ktrace_get_owning_pid();
    ktrace_unlock();
    if (owner == 0 && (kdebug_enable & KDEBUG_ENABLE_TRACE)) {
        return true;
    }
    os_atomic_store(&switch_recorder.owns_ring, false, relaxed);
    if (!switch_recorder.taken_logged) {
        switch_recorder.taken_logged = true;
        os_log(OS_LOG_DEFAULT, "SwitchRecorder: kdebug is off or another process owns it: no rings until a boot");
    }
    return false;
}

void switch_recorder_count_taken(void)
{
    os_atomic_inc(&switch_recorder.taken, relaxed);
}

/* Under the trigger lock: the capture call runs at `when` unless it is armed for sooner. */
static void switch_recorder_arm(thread_call_t call, uint64_t when)
{
    if (switch_recorder.armed != 0 && switch_recorder.armed <= when) {
        return;
    }
    switch_recorder.armed = when;
    thread_call_enter_delayed(call, when);
}

static bool switch_recorder_ask(struct switch_recorder_ask ask)
{
    thread_call_t call = os_atomic_load(&switch_recorder.call, acquire);
    const unsigned int reason = ask.reason;
    unsigned refusal = SWITCH_RECORDER_ADMITTED;

    if (reason == SWITCH_RECORDER_NONE || reason >= SWITCH_RECORDER_REASONS) {
        return false;
    }
    KDBG(SWITCH_RECORDER_KDBG(SWITCH_RECORDER_KDBG_TRIGGER), reason, ask.tid, ask.value);
    os_atomic_inc(&switch_recorder.triggers[reason], relaxed);
    if (call == NULL) {
        return false;
    }
    ask.at = mach_absolute_time();
    const boolean_t intrs = ml_set_interrupts_enabled(FALSE);
    simple_lock(&switch_recorder_trigger_lock, LCK_GRP_NULL);
    ask.seq = ++switch_recorder.seq;
    refusal = switch_recorder_admit(&switch_recorder.asks, &ask, &switch_recorder.recent,
                                    os_atomic_load(&switch_recorder.expect_tid, acquire),
                                    os_atomic_load(&switch_recorder.expect_until, relaxed));
    if (refusal == SWITCH_RECORDER_ADMITTED) {
        switch_recorder_arm(call, switch_recorder_due(&ask, switch_recorder.tail));
    } else {
        os_atomic_inc(&switch_recorder.refused[refusal], relaxed);
        if (switch_recorder.refusals < SWITCH_RECORDER_REFUSALS &&
            switch_recorder_refusal_logged(&switch_recorder.seen, &ask, switch_recorder.refusal_every)) {
            switch_recorder.refusal[switch_recorder.refusals] = ask;
            switch_recorder.refusal_why[switch_recorder.refusals++] = (uint8_t)refusal;
            switch_recorder_arm(call, ask.at);
        }
    }
    simple_unlock(&switch_recorder_trigger_lock);
    (void)ml_set_interrupts_enabled(intrs);
    return refusal == SWITCH_RECORDER_ADMITTED;
}

bool switch_recorder_trigger(unsigned int reason, uint64_t tid, uint64_t value)
{
    return switch_recorder_ask((struct switch_recorder_ask){.tid = tid, .value = value, .reason = reason});
}

/* watch.c: a UI main thread's hang, with its stretch's split so far. */
bool switch_recorder_hang(uint64_t tid, uint64_t ms, uint32_t on_ms, uint32_t blocked_ms, uint32_t runnable_ms)
{
    return switch_recorder_ask((struct switch_recorder_ask){.tid = tid,
                                                            .value = ms,
                                                            .reason = SWITCH_RECORDER_HANG,
                                                            .on_ms = on_ms,
                                                            .blocked_ms = blocked_ms,
                                                            .runnable_ms = runnable_ms});
}

/* capture.c's thread: the next due ask, taken out of the queue; when none is due, the call is armed for the next. */
bool switch_recorder_next(uint64_t now, struct switch_recorder_ask *out)
{
    thread_call_t call = os_atomic_load(&switch_recorder.call, acquire);
    uint64_t next = UINT64_MAX;
    bool got = false;

    const boolean_t intrs = ml_set_interrupts_enabled(FALSE);
    simple_lock(&switch_recorder_trigger_lock, LCK_GRP_NULL);
    switch_recorder.armed = 0;
    got = switch_recorder_take(&switch_recorder.asks, now, switch_recorder.tail, out, &next);
    if (!got && next != UINT64_MAX) {
        switch_recorder_arm(call, next);
    }
    simple_unlock(&switch_recorder_trigger_lock);
    (void)ml_set_interrupts_enabled(intrs);
    return got;
}

/* capture.c: the refusals not yet logged, at most `max`, and why each was refused. */
uint32_t switch_recorder_refusals(struct switch_recorder_ask *out, uint8_t *why, uint32_t max)
{
    uint32_t n = 0;

    const boolean_t intrs = ml_set_interrupts_enabled(FALSE);
    simple_lock(&switch_recorder_trigger_lock, LCK_GRP_NULL);
    n = switch_recorder.refusals < max ? switch_recorder.refusals : max;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = switch_recorder.refusal[i];
        why[i] = switch_recorder.refusal_why[i];
    }
    switch_recorder.refusals = 0;
    simple_unlock(&switch_recorder_trigger_lock);
    (void)ml_set_interrupts_enabled(intrs);
    return n;
}

void switch_recorder_late_frame(uint64_t interval_us)
{
    (void)switch_recorder_trigger(SWITCH_RECORDER_LATE_FRAME, 0, interval_us);
}

void switch_recorder_mark(void)
{
    (void)switch_recorder_trigger(SWITCH_RECORDER_MARK, 0, 0);
}

uint64_t switch_recorder_expected(void)
{
    return os_atomic_load(&switch_recorder.expect_tid, acquire);
}

/* store.c, after each capture it writes (`tid`: the thread it names): releases HW Test's hold when it was the held
 * thread's, and wakes the expect and wait sysctls, which wait for the store to go quiet. */
void switch_recorder_expect_done(uint64_t tid)
{
    if (tid != 0) {
        (void)os_atomic_cmpxchg(&switch_recorder.expect_tid, tid, 0, release);
    }
    thread_wakeup(&switch_recorder.expect_tid);
}

static int switch_recorder_status_out(struct sysctl_req *req)
{
    char line[900];
    struct switch_recorder_cost cost = {.ring = (uint32_t)kd_buffer_trace.kdb_event_count};
    struct switch_recorder_counts counts = {0};
    struct switch_recorder_last last = {0};
    uint64_t age_ms = 0, ns = 0;

    switch_recorder_store_counts(&counts, &last);
    for (unsigned int i = 0; i < SWITCH_RECORDER_REASONS; i++) {
        counts.triggers[i] = os_atomic_load(&switch_recorder.triggers[i], relaxed);
    }
    for (unsigned int i = 0; i < SWITCH_RECORDER_REFUSALS_KINDS; i++) {
        counts.refused[i] = os_atomic_load(&switch_recorder.refused[i], relaxed);
    }
    counts.taken = os_atomic_load(&switch_recorder.taken, relaxed);
    if (switch_recorder_owns_ring() && (kd_control_trace.kdc_live_flags & KDBG_WRAPPED) != 0) {
        const uint64_t oldest = os_atomic_load(&kd_control_trace.kdc_oldest_time, relaxed);
        const uint64_t now = kdebug_timestamp();

        if (oldest != 0 && now > oldest) {
            absolutetime_to_nanoseconds(now - oldest, &ns);
            cost.span_ms = ns / NSEC_PER_MSEC;
        }
    }
    cost.samples = switch_recorder_samples();
    if (last.head.done_at != 0) {
        absolutetime_to_nanoseconds(mach_absolute_time() - last.head.done_at, &ns);
        age_ms = ns / NSEC_PER_MSEC;
    }
    switch_recorder_status(line, sizeof(line), &cost, &counts, &last, age_ms);
    return SYSCTL_OUT(req, line, strlen(line) + 1);
}

static int switch_recorder_status_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
                                         struct sysctl_req *req)
{
    return switch_recorder_status_out(req);
}

SYSCTL_PROC(_debug, OID_AUTO, switch_recorder, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
            switch_recorder_status_sysctl, "A", "The flight recorder's cost, triggers and captures");

/* Sleeps on `event` until `done` holds or `deadline` passes: false when interrupted. */
static bool switch_recorder_sleep(event_t event, uint64_t deadline, bool (*done)(void))
{
    if (done()) {
        return true;
    }
    assert_wait_deadline(event, THREAD_ABORTSAFE, deadline);
    if (done()) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return true;
    }
    return thread_block(THREAD_CONTINUE_NULL) != THREAD_INTERRUPTED;
}

/* No ask waits and the store holds nothing unwritten. */
static bool switch_recorder_quiet(void)
{
    uint32_t asks = 0;

    const boolean_t intrs = ml_set_interrupts_enabled(FALSE);
    simple_lock(&switch_recorder_trigger_lock, LCK_GRP_NULL);
    asks = switch_recorder.asks.n;
    simple_unlock(&switch_recorder_trigger_lock);
    (void)ml_set_interrupts_enabled(intrs);
    return asks == 0 && switch_recorder_store_quiet();
}

static bool switch_recorder_expect_met(void)
{
    return os_atomic_load(&switch_recorder.expect_tid, acquire) == 0 ||
           (mach_absolute_time() >= os_atomic_load(&switch_recorder.expect_until, relaxed) && switch_recorder_quiet());
}

/*
 * HW Test's recorder test owns the next automatic capture: a write of its main thread's id waits until the store is
 * quiet (no ask waiting, nothing in memory unwritten), then for SWITCH_RECORDER_EXPECT_NS turns away every other
 * thread's automatic trigger and lets this thread's through the spacing. A read of debug.switch_recorder_wait then waits
 * for that thread's capture to be written (or the window's end) and returns the status line. Anybody may write it: it
 * captures nothing itself, and holds the recorder for five seconds at most.
 */
static int switch_recorder_expect_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
                                         struct sysctl_req *req)
{
    int64_t tid = 0;
    int changed = 0;
    int error = sysctl_io_number(req, 0, sizeof(tid), &tid, &changed);

    if (error || !changed) {
        return error;
    }
    if (tid <= 0 || os_atomic_load(&switch_recorder.call, acquire) == NULL) {
        return EINVAL;
    }
    while (!switch_recorder_quiet()) {
        if (!switch_recorder_sleep(&switch_recorder.expect_tid, mach_absolute_time() + switch_recorder.recent.gap,
                                   switch_recorder_quiet)) {
            return EINTR;
        }
    }
    os_atomic_store(&switch_recorder.expect_until, mach_absolute_time() + switch_recorder.expect_window, relaxed);
    os_atomic_store(&switch_recorder.expect_tid, (uint64_t)tid, release);
    return 0;
}

static int switch_recorder_wait_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
                                       struct sysctl_req *req)
{
    while (!switch_recorder_expect_met()) {
        const uint64_t until = os_atomic_load(&switch_recorder.expect_until, relaxed);
        const uint64_t now = mach_absolute_time();

        if (!switch_recorder_sleep(&switch_recorder.expect_tid, until > now ? until : now + switch_recorder.recent.gap,
                                   switch_recorder_expect_met)) {
            return EINTR;
        }
    }
    os_atomic_store(&switch_recorder.expect_tid, 0, release);
    return switch_recorder_status_out(req);
}

SYSCTL_PROC(_debug, OID_AUTO, switch_recorder_expect, CTLTYPE_QUAD | CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
            NULL, 0, switch_recorder_expect_sysctl, "Q", "Hold the flight recorder's next capture for this thread");
SYSCTL_PROC(_debug, OID_AUTO, switch_recorder_wait, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
            NULL, 0, switch_recorder_wait_sysctl, "A", "Wait for the held thread's capture, then the status line");

/* A mark or a user's slow work (ms), on the writing thread; root only. */
static int switch_recorder_user_trigger(__unused struct sysctl_oid *oidp, void *arg1, __unused int arg2,
                                        struct sysctl_req *req)
{
    const unsigned int reason = (unsigned int)(uintptr_t)arg1;
    int value = 0;
    int changed = 0;
    const int error = sysctl_io_number(req, 0, sizeof(value), &value, &changed);

    if (error || !changed) {
        return error;
    }
    if (value < 0 || (reason == SWITCH_RECORDER_SLOW_WORK && (unsigned int)value < SWITCH_RECORDER_SLOW_MS)) {
        return EINVAL;
    }
    (void)switch_recorder_trigger(reason, thread_tid(current_thread()), (uint64_t)value);
    return 0;
}

SYSCTL_PROC(_debug, OID_AUTO, switch_recorder_mark, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED,
            (void *)(uintptr_t)SWITCH_RECORDER_MARK, 0, switch_recorder_user_trigger, "I",
            "Mark this moment: capture the flight recorder");
SYSCTL_PROC(_debug, OID_AUTO, switch_recorder_slow, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED,
            (void *)(uintptr_t)SWITCH_RECORDER_SLOW_WORK, 0, switch_recorder_user_trigger, "I",
            "Work that took this many ms (100 or more): capture the flight recorder");

#endif /* SWITCH_RECORDER */
