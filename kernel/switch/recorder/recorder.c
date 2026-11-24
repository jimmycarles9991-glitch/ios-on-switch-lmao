// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder's triggers and dumps (--//kernel:recorder; <switch/recorder.h>).
 *
 * kdebug_startup starts kdebug wrapping through SWITCH_RECORDER_RING_EVENTS with switch_recorder_filter, owned by the
 * kernel, and kperf_init_early starts kperf's timer sampling the running thread's kernel and user stacks into the same
 * ring (sample.c). A trigger records one tracepoint and, unless a dump is in flight or the last ended less than
 * SWITCH_RECORDER_GAP_NS before, starts one on a thread call: at once for a hang, so the stackshot catches the hung
 * thread, else SWITCH_RECORDER_TAIL_NS later, so the ring holds what followed.
 *
 * A dump is expensive, which is why the gap is long and the triggers are few: its stackshot stops every CPU with
 * interrupts off for as long as it takes (last_shot_us), reading the ring turns tracing off until 16 MiB are written
 * (last_trace_us), and the two files put 16 to 24 MiB on the card. It goes to mobile's CrashReporter, owned by mobile
 * so `idevicecrashreport` copies it, in one of SWITCH_RECORDER_SLOTS slots taken in turn:
 * Switch-recorder-<slot>.ktrace (kdbg_dump_trace_to_file's RAW file: the ring, oldest first, with no thread map) and
 * Switch-recorder-<slot>.kcdata (a stackshot of every task, which names the ring's threads and resolves its user
 * addresses). A ring another process took (a ktrace session) is never dumped: its triggers are counted as taken.
 */

#include <switch/recorder.h>

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/kdebug.h>
#include <sys/kdebug_common.h>
#include <sys/kdebug_private.h>
#include <sys/ktrace.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vnode_internal.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/simple_lock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <kern/thread_call.h>
#include <os/atomic_private.h>
#include <os/log.h>
#include <string.h>

#include "recorder.h"

#if SWITCH_RECORDER

/*
 * Scheduler, interrupts, Mach traps and BSD system calls, VM faults, file system reads and disk I/O (page-ins with
 * their vnodes), shared-region events, turnstiles, kperf's samples, our display and GPU tracepoints, and the signpost
 * classes Apple's processes may emit. Left out: Mach IPC (MACH_IPC_KMSG_INFO takes the destination port's lock on every
 * send in every process) and the Metal plugin's DBG_APPS 'S' points (they would turn into system calls on its render
 * thread); the kernel's flip and job tracepoints carry the frame instead.
 */
char switch_recorder_filter[] = "S0x0140,S0x0105,S0x010c,S0x0130,S0x01a8,S0x040c,S0x0301,S0x0302,C0x25,S0x0612,"
                                "S0x210a,C0x2b,C0x35";
bool switch_recorder_own_filter;

extern struct kd_control kd_control_trace;
extern struct kd_buffer kd_buffer_trace;

#define SWITCH_RECORDER_DIR        "/private/var/mobile/Library/Logs/CrashReporter"
#define SWITCH_RECORDER_MOBILE     501
#define SWITCH_RECORDER_SHOT_BYTES (4u << 20)
#define SWITCH_RECORDER_SHOT_MAX   (8u << 20) /* stackshot's cap (max_tracebuf_size) */
#define SWITCH_RECORDER_SHOT_FLAGS                                                                 \
    ((uint64_t)(STACKSHOT_KCDATA_FORMAT | STACKSHOT_SAVE_LOADINFO | STACKSHOT_SAVE_KEXT_LOADINFO | \
                STACKSHOT_THREAD_WAITINFO | STACKSHOT_NO_IO_STATS | STACKSHOT_INCLUDE_DRIVER_THREADS_IN_KERNEL))

static struct {
    thread_call_t call; /* published with tail, gap and expect_window (release); NULL until kperf is configured */
    uint64_t tail, gap, expect_window;
    uint32_t state;      /* recorder.h: idle, claimed, a queued reason, or RUNNING | the reason */
    uint64_t tid, value; /* the queued dump's, written by its claimer before it queues the reason */
    thread_t dumper;     /* the dump thread inside kdbg_dump_trace_to_file: only it ever compares equal to itself */
    uint64_t gap_from;   /* the last dump's end, 0: none */
    uint64_t expect_tid, expect_until;
    bool owns_ring; /* the ring is the one kdebug_startup started for the recorder, never taken since */
    bool taken_logged;
    struct switch_recorder_counts counts;
    uint32_t seq; /* odd while the dump thread rewrites gate and last */
    struct switch_recorder_gate gate;
    struct switch_recorder_last last;
    int64_t raw_offset; /* 10063's RAW_file_offset and RAW_file_written around a dump */
    int raw_written;
} switch_recorder;

static SIMPLE_LOCK_DECLARE(switch_recorder_trigger_lock, 0);

static void switch_recorder_dump(thread_call_param_t p0, thread_call_param_t p1);

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
    thread_call_t call = thread_call_allocate_with_options(switch_recorder_dump, NULL, THREAD_CALL_PRIORITY_USER,
                                                           THREAD_CALL_OPTIONS_ONCE);

    nanoseconds_to_absolutetime(SWITCH_RECORDER_TAIL_NS, &switch_recorder.tail);
    nanoseconds_to_absolutetime(SWITCH_RECORDER_GAP_NS, &switch_recorder.gap);
    nanoseconds_to_absolutetime(SWITCH_RECORDER_EXPECT_NS, &switch_recorder.expect_window);
    os_atomic_store(&switch_recorder.call, call, release);
}

bool switch_recorder_dumping(void)
{
    return os_atomic_load(&switch_recorder.dumper, relaxed) == current_thread();
}

bool switch_recorder_owns_ring(void)
{
    return os_atomic_load(&switch_recorder.owns_ring, relaxed);
}

/* 10063's kdbg_dump_trace_to_file, under ktrace_lock. */
void switch_recorder_raw_begin(int64_t *offset, int *written)
{
    if (!switch_recorder_dumping()) {
        return;
    }
    switch_recorder.raw_offset = *offset;
    switch_recorder.raw_written = *written;
    *offset = 0;
    *written = 0;
}

void switch_recorder_raw_end(int64_t *offset, int *written)
{
    if (!switch_recorder_dumping()) {
        return;
    }
    *offset = switch_recorder.raw_offset;
    *written = switch_recorder.raw_written;
}

bool switch_recorder_trigger(unsigned int reason, uint64_t tid, uint64_t value)
{
    thread_call_t call = os_atomic_load(&switch_recorder.call, acquire);
    uint64_t now = 0, expect = 0;
    uint32_t state = 0;

    if (reason == SWITCH_RECORDER_NONE || reason >= SWITCH_RECORDER_REASONS) {
        return false;
    }
    KDBG(SWITCH_RECORDER_KDBG(SWITCH_RECORDER_KDBG_TRIGGER), reason, tid, value);
    os_atomic_inc(&switch_recorder.counts.triggers[reason], relaxed);
    if (call == NULL) {
        return false;
    }
    now = mach_absolute_time();
    if (switch_recorder_in_gap(os_atomic_load(&switch_recorder.gap_from, acquire), now, switch_recorder.gap)) {
        os_atomic_inc(&switch_recorder.counts.skipped, relaxed);
        return false;
    }
    expect = os_atomic_load(&switch_recorder.expect_tid, acquire);
    if (expect != 0 && expect != tid && now < os_atomic_load(&switch_recorder.expect_until, relaxed)) {
        os_atomic_inc(&switch_recorder.counts.held, relaxed);
        return false;
    }
    const boolean_t intrs = ml_set_interrupts_enabled(FALSE);
    simple_lock(&switch_recorder_trigger_lock, LCK_GRP_NULL);
    state = os_atomic_load(&switch_recorder.state, relaxed);
    do {
        if (!switch_recorder_takes(state, reason)) {
            simple_unlock(&switch_recorder_trigger_lock);
            (void)ml_set_interrupts_enabled(intrs);
            return false;
        }
    } while (!os_atomic_cmpxchgv(&switch_recorder.state, state, SWITCH_RECORDER_CLAIM, &state, acquire));
    switch_recorder.tid = tid;
    switch_recorder.value = value;
    os_atomic_store(&switch_recorder.state, reason, release);
    if (reason == SWITCH_RECORDER_HANG) {
        thread_call_enter(call);
    } else {
        thread_call_enter_delayed(call, now + switch_recorder.tail);
    }
    simple_unlock(&switch_recorder_trigger_lock);
    (void)ml_set_interrupts_enabled(intrs);
    return true;
}

void switch_recorder_late_frame(uint64_t interval_us)
{
    (void)switch_recorder_trigger(SWITCH_RECORDER_LATE_FRAME, 0, interval_us);
}

void switch_recorder_mark(void)
{
    (void)switch_recorder_trigger(SWITCH_RECORDER_MARK, 0, 0);
}

static uint64_t switch_recorder_us(uint64_t since)
{
    uint64_t ns = 0;

    absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
    return ns / NSEC_PER_USEC;
}

/* An empty file at path, owned by mobile and readable by all, left open for writing in *vp. */
static int switch_recorder_create(const char *path, vnode_t *vp, vfs_context_t ctx)
{
    struct vnode_attr va;
    int error = vnode_open(path, O_CREAT | FWRITE | O_NOFOLLOW, 0644, 0, vp, ctx);

    if (error) {
        return error;
    }
    VATTR_INIT(&va);
    VATTR_SET(&va, va_data_size, 0);
    VATTR_SET(&va, va_uid, SWITCH_RECORDER_MOBILE);
    VATTR_SET(&va, va_gid, SWITCH_RECORDER_MOBILE);
    VATTR_SET(&va, va_mode, 0644);
    error = vnode_setattr(*vp, &va, ctx);
    if (error) {
        vnode_close(*vp, FWRITE, ctx);
        *vp = NULLVP;
    }
    return error;
}

/* The ring as kdbg_dump_trace_to_file writes it: its bytes, 0 when nothing was written. */
static uint64_t switch_recorder_trace(const char *path, vfs_context_t ctx)
{
    const bool was_eager = thread_is_eager_preempt(current_thread());
    vnode_t vp = NULLVP;
    off_t size = 0;

    if (switch_recorder_create(path, &vp, ctx) != 0) {
        return 0;
    }
    vnode_close(vp, FWRITE, ctx);
    /* kdbg_dump_trace_to_file unconditionally calls thread_set_eager_preempt(), which asserts it is unset;
     * THREAD_CALL_PRIORITY_USER callout threads inherit TH_SFLAG_EAGERPREEMPT from thread_call_thread_create(). */
    if (was_eager) {
        thread_clear_eager_preempt(current_thread());
    }
    os_atomic_store(&switch_recorder.dumper, current_thread(), relaxed);
    kdbg_dump_trace_to_file(path, true);
    os_atomic_store(&switch_recorder.dumper, THREAD_NULL, relaxed);
    if (was_eager && !thread_is_eager_preempt(current_thread())) {
        thread_set_eager_preempt(current_thread());
    }
    if (vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &vp, ctx) == 0) {
        (void)vnode_size(vp, &size, ctx);
        vnode_put(vp);
    }
    return size > 0 ? (uint64_t)size : 0;
}

/* A stackshot of every task into path: its bytes, 0 when none was written. No file, no stackshot: it stops every CPU. */
static uint64_t switch_recorder_shot(const char *path, vfs_context_t ctx)
{
    uint32_t size = SWITCH_RECORDER_SHOT_BYTES;
    unsigned bytes = 0;
    kern_return_t kr = KERN_FAILURE;
    void *buf = NULL;
    vnode_t vp = NULLVP;
    int resid = 0;
    int error = 0;

    if (switch_recorder_create(path, &vp, ctx) != 0) {
        return 0;
    }
    for (;;) {
        buf = kalloc_data(size, Z_WAITOK);
        if (buf == NULL) {
            vnode_close(vp, FWRITE, ctx);
            return 0;
        }
        bytes = 0;
        kr = stack_snapshot_from_kernel(-1, buf, size, SWITCH_RECORDER_SHOT_FLAGS, 0, 0, &bytes);
        if (kr != KERN_INSUFFICIENT_BUFFER_SIZE || size >= SWITCH_RECORDER_SHOT_MAX) {
            break;
        }
        kfree_data(buf, size);
        size *= 2;
    }
    if (kr == KERN_SUCCESS && bytes != 0) {
        error = vn_rdwr(UIO_WRITE, vp, (caddr_t)buf, (int)bytes, 0, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT,
                        vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    }
    vnode_close(vp, FWRITE, ctx);
    kfree_data(buf, size);
    return kr != KERN_SUCCESS || error || resid ? 0 : bytes;
}

static bool switch_recorder_ring_owned(void)
{
    int owner = 0;

    ktrace_lock();
    owner = ktrace_get_owning_pid();
    ktrace_unlock();
    return owner == 0 && (kdebug_enable & KDEBUG_ENABLE_TRACE);
}

/* The dump thread's alone: preemption off, so a status read never waits on a preempted writer. */
static void switch_recorder_publish(const struct switch_recorder_last *last, bool dumped)
{
    disable_preemption();
    os_atomic_store(&switch_recorder.seq, switch_recorder.seq + 1, relaxed);
    os_atomic_thread_fence(release);
    if (dumped) {
        switch_recorder.last = *last;
        switch_recorder_gate_done(&switch_recorder.gate);
    }
    os_atomic_store(&switch_recorder.seq, switch_recorder.seq + 1, release);
    enable_preemption();
}

static void switch_recorder_finish(uint64_t tid)
{
    os_atomic_store(&switch_recorder.gap_from, mach_absolute_time(), release);
    if (tid != 0 && os_atomic_load(&switch_recorder.expect_tid, acquire) == tid) {
        os_atomic_store(&switch_recorder.expect_tid, 0, release);
        thread_wakeup(&switch_recorder.expect_tid);
    }
    os_atomic_store(&switch_recorder.state, SWITCH_RECORDER_IDLE, release);
    thread_wakeup(&switch_recorder.state);
}

static void switch_recorder_dump(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    const uint32_t reason = os_atomic_load(&switch_recorder.state, acquire);
    vfs_context_t ctx = vfs_context_kernel();
    struct switch_recorder_last last = {0};
    char trace[96], shot[96];
    uint64_t t = 0;

    /* Idle, a claim still writing, or a hang that took this reason's place: that claimer enters the call again. */
    if (reason == SWITCH_RECORDER_IDLE || reason >= SWITCH_RECORDER_REASONS ||
        !os_atomic_cmpxchg(&switch_recorder.state, reason, SWITCH_RECORDER_RUNNING | reason, acquire)) {
        return;
    }
    last.reason = reason;
    last.tid = switch_recorder.tid;
    last.value = switch_recorder.value;
    last.slot = switch_recorder.gate.slot;
    if (!switch_recorder_owns_ring() || !switch_recorder_ring_owned()) {
        os_atomic_inc(&switch_recorder.counts.taken, relaxed);
        os_atomic_store(&switch_recorder.owns_ring, false, relaxed);
        if (!switch_recorder.taken_logged) {
            os_log(OS_LOG_DEFAULT, "SwitchRecorder: kdebug is off or another process owns it: no dumps until a boot");
            switch_recorder.taken_logged = true;
        }
        switch_recorder_publish(&last, false);
        switch_recorder_finish(0);
        return;
    }
    snprintf(trace, sizeof(trace), SWITCH_RECORDER_DIR "/Switch-recorder-%u.ktrace", last.slot);
    snprintf(shot, sizeof(shot), SWITCH_RECORDER_DIR "/Switch-recorder-%u.kcdata", last.slot);
    KDBG(SWITCH_RECORDER_KDBG(SWITCH_RECORDER_KDBG_DUMP) | DBG_FUNC_START, reason, last.slot);
    /* A hang's stackshot first, while the thread still hangs; any other's after the ring, which it would perturb. */
    if (reason == SWITCH_RECORDER_HANG) {
        t = mach_absolute_time();
        last.shot_bytes = switch_recorder_shot(shot, ctx);
        last.shot_us = switch_recorder_us(t);
    }
    t = mach_absolute_time();
    last.trace_bytes = switch_recorder_trace(trace, ctx);
    last.trace_us = switch_recorder_us(t);
    if (reason != SWITCH_RECORDER_HANG) {
        t = mach_absolute_time();
        last.shot_bytes = switch_recorder_shot(shot, ctx);
        last.shot_us = switch_recorder_us(t);
    }
    KDBG(SWITCH_RECORDER_KDBG(SWITCH_RECORDER_KDBG_DUMP) | DBG_FUNC_END, reason, last.slot);
    switch_recorder_publish(&last, true);
    os_log(OS_LOG_DEFAULT,
           "SwitchRecorder: dump %u (%s, thread %llu, value %llu) in slot %u: ring %llu KiB in %llu us, stackshot %llu "
           "KiB in %llu us",
           switch_recorder.gate.dumps, switch_recorder_reason_name(reason), last.tid, last.value, last.slot,
           last.trace_bytes / 1024, last.trace_us, last.shot_bytes / 1024, last.shot_us);
    switch_recorder_finish(last.tid);
}

static int switch_recorder_status_out(struct sysctl_req *req)
{
    char line[640];
    struct switch_recorder_cost cost = {.ring = (uint32_t)kd_buffer_trace.kdb_event_count};
    struct switch_recorder_counts counts = {0};
    struct switch_recorder_gate gate;
    struct switch_recorder_last last;
    const uint64_t from = os_atomic_load(&switch_recorder.gap_from, acquire);
    uint64_t age_ms = 0, ns = 0;
    uint32_t seq = 0;

    do {
        seq = os_atomic_load(&switch_recorder.seq, acquire);
        gate = switch_recorder.gate;
        last = switch_recorder.last;
        os_atomic_thread_fence(acquire);
    } while ((seq & 1) != 0 || seq != os_atomic_load(&switch_recorder.seq, relaxed));
    for (unsigned int i = 0; i < SWITCH_RECORDER_REASONS; i++) {
        counts.triggers[i] = os_atomic_load(&switch_recorder.counts.triggers[i], relaxed);
    }
    counts.skipped = os_atomic_load(&switch_recorder.counts.skipped, relaxed);
    counts.held = os_atomic_load(&switch_recorder.counts.held, relaxed);
    counts.taken = os_atomic_load(&switch_recorder.counts.taken, relaxed);
    if (switch_recorder_owns_ring() && (kd_control_trace.kdc_live_flags & KDBG_WRAPPED) != 0) {
        const uint64_t oldest = os_atomic_load(&kd_control_trace.kdc_oldest_time, relaxed);
        const uint64_t now = kdebug_timestamp();

        if (oldest != 0 && now > oldest) {
            absolutetime_to_nanoseconds(now - oldest, &ns);
            cost.span_ms = ns / NSEC_PER_MSEC;
        }
    }
    cost.samples = switch_recorder_samples();
    if (from != 0) {
        absolutetime_to_nanoseconds(mach_absolute_time() - from, &ns);
        age_ms = ns / NSEC_PER_MSEC;
    }
    switch_recorder_status(line, sizeof(line), &cost, &counts, &gate, &last, age_ms);
    return SYSCTL_OUT(req, line, strlen(line) + 1);
}

static int switch_recorder_status_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
                                         struct sysctl_req *req)
{
    return switch_recorder_status_out(req);
}

SYSCTL_PROC(_debug, OID_AUTO, switch_recorder, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
            switch_recorder_status_sysctl, "A", "The flight recorder's cost, triggers and dumps");

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

static bool switch_recorder_free(void)
{
    return os_atomic_load(&switch_recorder.state, acquire) == SWITCH_RECORDER_IDLE &&
           !switch_recorder_in_gap(os_atomic_load(&switch_recorder.gap_from, acquire), mach_absolute_time(),
                                   switch_recorder.gap);
}

static bool switch_recorder_expect_met(void)
{
    return os_atomic_load(&switch_recorder.expect_tid, acquire) == 0 ||
           (mach_absolute_time() >= os_atomic_load(&switch_recorder.expect_until, relaxed) &&
            os_atomic_load(&switch_recorder.state, acquire) == SWITCH_RECORDER_IDLE);
}

/*
 * HW Test's recorder test owns the next dump: a write of its main thread's id waits until no dump is in flight and the
 * gap has passed, then for SWITCH_RECORDER_EXPECT_NS turns away every other thread's trigger. A read of
 * debug.switch_recorder_wait then waits for that thread's dump (or the window's end) and returns the status line.
 * Anybody may write it: it dumps nothing itself, and holds the recorder for five seconds at most.
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
    while (!switch_recorder_free()) {
        const uint64_t from = os_atomic_load(&switch_recorder.gap_from, acquire);
        const uint64_t now = mach_absolute_time();
        const uint64_t end = from != 0 ? from + switch_recorder.gap : 0;
        const uint64_t deadline = end > now ? end : now + switch_recorder.gap;

        if (!switch_recorder_sleep(&switch_recorder.state, deadline, switch_recorder_free)) {
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
        const bool window = mach_absolute_time() < os_atomic_load(&switch_recorder.expect_until, relaxed);
        event_t event = window ? (event_t)&switch_recorder.expect_tid : (event_t)&switch_recorder.state;
        const uint64_t deadline = window ? os_atomic_load(&switch_recorder.expect_until, relaxed)
                                         : mach_absolute_time() + switch_recorder.gap;

        if (!switch_recorder_sleep(event, deadline, switch_recorder_expect_met)) {
            return EINTR;
        }
    }
    os_atomic_store(&switch_recorder.expect_tid, 0, release);
    return switch_recorder_status_out(req);
}

SYSCTL_PROC(_debug, OID_AUTO, switch_recorder_expect, CTLTYPE_QUAD | CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
            NULL, 0, switch_recorder_expect_sysctl, "Q", "Hold the flight recorder's next dump for this thread");
SYSCTL_PROC(_debug, OID_AUTO, switch_recorder_wait, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
            NULL, 0, switch_recorder_wait_sysctl, "A", "Wait for the held thread's dump, then the status line");

/* A mark or a user's slow work (ms), on the writing thread; root only, since each dump stops every CPU. */
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
            "Mark this moment: dump the flight recorder");
SYSCTL_PROC(_debug, OID_AUTO, switch_recorder_slow, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED,
            (void *)(uintptr_t)SWITCH_RECORDER_SLOW_WORK, 0, switch_recorder_user_trigger, "I",
            "Work that took this many ms (100 or more): dump the flight recorder");

#endif /* SWITCH_RECORDER */
