// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder's capture (--//kernel:recorder; state.h): a kernel thread at SWITCH_RECORDER_CAPTURE_PRI, woken
 * by recorder.c's thread call, that turns each due ask into a capture in memory, a stackshot and the ring, and hands
 * it to the store (store.c), which writes it later at low priority. On 25 Nov the dump's thread call, at the user
 * priority, ran 13 s after its trigger at boot and then kept tracing off for the 56 s it took to write the ring: a
 * capture must not wait behind the load it records, and must not write while the moment it records lasts.
 *
 * A hang's stackshot is taken first, while its thread still hangs; any other's after the ring, which the stackshot's
 * stop of every CPU (about 3 ms on 25 Nov) would otherwise end. Reading the ring turns tracing off for that read alone
 * (switch_recorder_kdebug_read), and empties it: the next capture's ring starts where this one ends.
 */

#include <switch/recorder.h>

#include <sys/errno.h>
#include <sys/kdebug.h>
#include <sys/kdebug_common.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <os/log.h>
#include <string.h>
#include <uuid/uuid.h>

#include "state.h"

#if SWITCH_RECORDER

/* Fixed priority, between SpringBoard's main thread and the boot's launch storm (47 and under, where the 25 Nov dump's
 * thread call starved) and the display's primary thread and the render server (54, 55), which it must not delay. */
#define SWITCH_RECORDER_CAPTURE_PRI 52
#define SWITCH_RECORDER_SHOT_BYTES  (4u << 20)
#define SWITCH_RECORDER_SHOT_MAX    (8u << 20) /* stackshot's cap (max_tracebuf_size) */
#define SWITCH_RECORDER_SHOT_FLAGS                                                                 \
    ((uint64_t)(STACKSHOT_KCDATA_FORMAT | STACKSHOT_SAVE_LOADINFO | STACKSHOT_SAVE_KEXT_LOADINFO | \
                STACKSHOT_THREAD_WAITINFO | STACKSHOT_NO_IO_STATS | STACKSHOT_INCLUDE_DRIVER_THREADS_IN_KERNEL))
/* The ring's file starts with a RAW_VERSION1 header and no thread or CPU map, its events at the next 4 KiB. */
#define SWITCH_RECORDER_RAW_PAGE 4096u
#define SWITCH_RECORDER_RAW_V1   0x55aa0101u

extern struct kd_buffer kd_buffer_trace;
/* osfmk's, private to Mach; bsd/kern/kern_memorystatus.c declares it the same way for its own threads. */
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
                                                  thread_t *new_thread);
extern uuid_string_t bootsessionuuid_string;
extern char kernel_uuid_string[37];

static uint32_t switch_recorder_wake; /* set by the thread call, taken by the capture thread */

static void switch_recorder_capture_loop(void *param, wait_result_t wr);

struct switch_recorder_raw_header {
    int32_t version_no;
    int32_t thread_count;
    uint64_t tod_secs;
    uint32_t tod_usecs;
};

static uint64_t switch_recorder_us(uint64_t from, uint64_t to)
{
    uint64_t ns = 0;

    absolutetime_to_nanoseconds(to > from ? to - from : 0, &ns);
    return ns / NSEC_PER_USEC;
}

static void switch_recorder_log_refusals(void)
{
    struct switch_recorder_ask ask[SWITCH_RECORDER_REFUSALS];
    uint8_t why[SWITCH_RECORDER_REFUSALS];
    const uint32_t n = switch_recorder_refusals(ask, why, SWITCH_RECORDER_REFUSALS);

    for (uint32_t i = 0; i < n; i++) {
        os_log(OS_LOG_DEFAULT, "SwitchRecorder: refused %s ask %u (thread %llu, value %llu) at %llu: %s",
               switch_recorder_reason_name(ask[i].reason), ask[i].seq, ask[i].tid, ask[i].value, ask[i].at,
               switch_recorder_refusal_name(why[i]));
    }
}

/* Every task's stackshot into memory, kept at its own size. */
static void switch_recorder_shot(struct switch_recorder_capture *c, bool mark)
{
    struct switch_recorder_head *h = &c->head;
    uint32_t size = SWITCH_RECORDER_SHOT_BYTES;
    kern_return_t kr = KERN_FAILURE;
    unsigned bytes = 0;
    void *buf = NULL;
    uint64_t t = 0;

    for (;;) {
        if (!switch_recorder_store_room(size, mark)) {
            return;
        }
        buf = kalloc_data(size, Z_WAITOK);
        if (buf == NULL) {
            switch_recorder_store_unroom(size);
            return;
        }
        bytes = 0;
        t = mach_absolute_time();
        kr = stack_snapshot_from_kernel(-1, buf, size, SWITCH_RECORDER_SHOT_FLAGS, 0, 0, &bytes);
        h->shot_at = mach_absolute_time();
        h->shot_us = switch_recorder_us(t, h->shot_at);
        if (kr != KERN_INSUFFICIENT_BUFFER_SIZE || size >= SWITCH_RECORDER_SHOT_MAX) {
            break;
        }
        kfree_data(buf, size);
        switch_recorder_store_unroom(size);
        size *= 2;
    }
    if (kr == KERN_SUCCESS && bytes != 0 && (c->shot = kalloc_data(bytes, Z_WAITOK)) != NULL &&
        switch_recorder_store_room(bytes, mark)) {
        memcpy(c->shot, buf, bytes);
        c->shot_alloc = bytes;
        h->shot_bytes = bytes;
    } else if (c->shot != NULL) {
        kfree_data(c->shot, bytes);
        c->shot = NULL;
    }
    kfree_data(buf, size);
    switch_recorder_store_unroom(size);
}

/* The ring into memory as a RAW_VERSION1 file, or why there is none (head.ring_lost). */
static void switch_recorder_ring(struct switch_recorder_capture *c, bool mark)
{
    struct switch_recorder_head *h = &c->head;
    const size_t want = SWITCH_RECORDER_RAW_PAGE + (size_t)kd_buffer_trace.kdb_event_count * sizeof(kd_buf);
    struct switch_recorder_raw_header raw = {.version_no = (int32_t)SWITCH_RECORDER_RAW_V1};
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    size_t events = 0;
    void *buf = NULL;
    int error = 0;

    if (!switch_recorder_ring_ours()) {
        h->ring_lost = 1;
        switch_recorder_count_taken();
        return;
    }
    if (!switch_recorder_store_room(want, mark)) {
        h->ring_lost = 2;
        return;
    }
    if ((buf = kalloc_data(want, Z_WAITOK)) == NULL) {
        switch_recorder_store_unroom(want);
        h->ring_lost = 2;
        return;
    }
    h->frozen_at = mach_absolute_time();
    clock_get_calendar_microtime(&secs, &usecs);
    switch_recorder_dumping_set(true);
    error =
        switch_recorder_kdebug_read((char *)buf + SWITCH_RECORDER_RAW_PAGE, want - SWITCH_RECORDER_RAW_PAGE, &events);
    switch_recorder_dumping_set(false);
    h->ring_us = switch_recorder_us(h->frozen_at, mach_absolute_time());
    if (error != 0) {
        kfree_data(buf, want);
        switch_recorder_store_unroom(want);
        h->ring_lost = 1;
        return;
    }
    raw.tod_secs = secs;
    raw.tod_usecs = usecs;
    memset(buf, 0, SWITCH_RECORDER_RAW_PAGE);
    memcpy(buf, &raw, sizeof(raw));
    h->tod_s = secs;
    h->tod_us = usecs;
    h->ring_bytes = SWITCH_RECORDER_RAW_PAGE + events * sizeof(kd_buf);
    c->ring = buf;
    c->ring_alloc = want;
}

/* recorder.c's start: the capture thread. */
bool switch_recorder_capture_init(void)
{
    thread_t thread = THREAD_NULL;

    if (kernel_thread_start_priority(switch_recorder_capture_loop, NULL, SWITCH_RECORDER_CAPTURE_PRI, &thread) !=
        KERN_SUCCESS) {
        return false;
    }
    thread_set_thread_name(thread, "switch_recorder_capture");
    thread_deallocate(thread);
    return true;
}

static const char *switch_recorder_lost(uint32_t why)
{
    switch (why) {
    case 1:
        return "; no ring: kdebug is off or another process owns it";
    case 2:
        return "; no ring: no memory for it";
    default:
        return "";
    }
}

static void switch_recorder_capture_one(const struct switch_recorder_ask *ask)
{
    struct switch_recorder_capture *c = kalloc_type(struct switch_recorder_capture, Z_WAITOK | Z_ZERO);
    const bool mark = ask->reason == SWITCH_RECORDER_MARK;
    struct switch_recorder_head h;

    if (c == NULL) {
        os_log(OS_LOG_DEFAULT, "SwitchRecorder: %s ask %u (thread %llu, value %llu) at %llu: no memory for a capture",
               switch_recorder_reason_name(ask->reason), ask->seq, ask->tid, ask->value, ask->at);
        return;
    }
    c->head.seq = switch_recorder_store_begin();
    c->head.ask = ask->seq;
    c->head.reason = ask->reason;
    c->head.tid = ask->tid;
    c->head.value = ask->value;
    c->head.at = ask->at;
    c->head.on_ms = ask->on_ms;
    c->head.blocked_ms = ask->blocked_ms;
    c->head.runnable_ms = ask->runnable_ms;
    strlcpy(c->head.boot, bootsessionuuid_string, sizeof(c->head.boot));
    strlcpy(c->head.kernel, kernel_uuid_string, sizeof(c->head.kernel));
    if (ask->reason == SWITCH_RECORDER_HANG) {
        switch_recorder_shot(c, mark);
        switch_recorder_ring(c, mark);
    } else {
        switch_recorder_ring(c, mark);
        switch_recorder_shot(c, mark);
    }
    c->head.done_at = mach_absolute_time();
    h = c->head;
    if (!mark && c->shot == NULL && c->ring == NULL && h.ring_lost == 2) {
        switch_recorder_store_abandon(c);
        os_log(OS_LOG_DEFAULT,
               "SwitchRecorder: capture %u (%s ask %u, thread %llu) dropped: the captures waiting for the store fill "
               "its memory",
               h.seq, switch_recorder_reason_name(h.reason), h.ask, h.tid);
        return;
    }
    switch_recorder_store_push(c);
    os_log(OS_LOG_DEFAULT,
           "SwitchRecorder: capture %u (%s ask %u, thread %llu, value %llu) at %llu: stackshot %llu KiB in %llu us "
           "(+%llu us), ring %llu KiB in %llu us (+%llu us), in memory +%llu us%s; its stretch: on a core %u ms, "
           "blocked %u ms, waiting for a core %u ms",
           h.seq, switch_recorder_reason_name(h.reason), h.ask, h.tid, h.value, h.at, h.shot_bytes >> 10, h.shot_us,
           switch_recorder_us(h.at, h.shot_at), h.ring_bytes >> 10, h.ring_us, switch_recorder_us(h.at, h.frozen_at),
           switch_recorder_us(h.at, h.done_at), switch_recorder_lost(h.ring_lost), h.on_ms, h.blocked_ms,
           h.runnable_ms);
}

/* recorder.c's thread call, at an ask's due time or a refusal: wakes the capture thread. */
void switch_recorder_capture_call(__unused void *p0, __unused void *p1)
{
    os_atomic_store(&switch_recorder_wake, 1, release);
    thread_wakeup((event_t)&switch_recorder_wake);
}

/* The wake flag is set before the wakeup and taken after the wait is asserted, so no wake between them is lost. */
static void switch_recorder_capture_loop(__unused void *param, __unused wait_result_t wr)
{
    struct switch_recorder_ask ask;

    for (;;) {
        assert_wait((event_t)&switch_recorder_wake, THREAD_UNINT);
        if (os_atomic_xchg(&switch_recorder_wake, 0, acquire) == 0) {
            (void)thread_block(THREAD_CONTINUE_NULL);
            continue;
        }
        clear_wait(current_thread(), THREAD_AWAKENED);
        switch_recorder_log_refusals();
        while (switch_recorder_next(mach_absolute_time(), &ask)) {
            switch_recorder_capture_one(&ask);
            switch_recorder_log_refusals();
        }
    }
}

#endif /* SWITCH_RECORDER */
