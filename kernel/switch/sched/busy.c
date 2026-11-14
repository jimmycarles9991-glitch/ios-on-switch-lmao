// SPDX-License-Identifier: GPL-2.0-only
/*
 * UI main threads' busy stretches (busy.h), debug kernels: one "SwitchBusy:" line for each stretch of SpringBoard's or
 * backboardd's main thread of SWITCH_BUSY_MIN_MS or more between two waits in its run loop, the stretches hangtracer
 * reports as hangs. The line splits the stretch into on a core, blocked by wait kind and waiting for a core, and names its
 * longest waits.
 *
 * The thread's departures (thread_dispatch, under its lock) and dispatches (thread_invoke, under its lock) are the only
 * writers of its state, and never run at once. A finished stretch goes into the slot's ring, which the departure path
 * alone fills and the writer alone drains (one release store each); the scheduler's maintenance tick puts the writer
 * on a thread call, since os_log cannot run under a thread lock.
 */

#include <switch/sched.h>
#include <switch/xnu.h>

#include "../diag/stall.h"
#include "busy.h"

#include <arm/thread.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/recount.h>
#include <kern/simple_lock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/waitq.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <ptrauth.h>
#include <string.h>
#include <vm/vm_shared_region.h>
#if XNU_12377_121_6
#include <vm/vm_shared_region_xnu.h>
#endif

#if DEVELOPMENT || DEBUG

#define SWITCH_BUSY_MIN_MS 250
#define BUSY_RING          4
#define BUSY_LINE          900 /* under the 1 KiB os_log record */
#define BUSY_KINDS         4

struct busy_slot {
    const char *name;
    uint64_t tid; /* the main thread of the last exec of `name`, 0 before one */
    int pid;
    struct switch_busy state;
    struct switch_busy_episode ring[BUSY_RING];
    uint32_t head; /* written by the departure path */
    uint32_t tail; /* written by the writer */
    uint32_t dropped;
};

static struct busy_slot busy_slots[] = {
    {.name = "SpringBoard"},
    {.name = "backboardd"},
};

uint32_t switch_sched_busy_watching;
static SIMPLE_LOCK_DECLARE(busy_lock, 0);
static thread_call_t busy_call;
static uint64_t busy_min;

static void busy_write(thread_call_param_t p0, thread_call_param_t p1);

static struct busy_slot *busy_slot_of(thread_t thread)
{
    for (unsigned int i = 0; i < sizeof(busy_slots) / sizeof(busy_slots[0]); i++) {
        if (os_atomic_load(&busy_slots[i].tid, acquire) == thread->thread_id) {
            return &busy_slots[i];
        }
    }
    return NULL;
}

/* kern_exec.c (through switch_stall_exec): the new main thread of a process, before it first runs. */
void switch_sched_busy_exec(thread_t thread)
{
    char name[32];

    switch_task_name(get_threadtask(thread), name, sizeof(name));
    for (unsigned int i = 0; i < sizeof(busy_slots) / sizeof(busy_slots[0]); i++) {
        struct busy_slot *s = &busy_slots[i];

        if (strcmp(name, s->name) != 0) {
            continue;
        }
        if (os_atomic_load(&busy_call, acquire) == NULL) {
            thread_call_t call = thread_call_allocate_with_options(busy_write, NULL, THREAD_CALL_PRIORITY_KERNEL,
                                                                   THREAD_CALL_OPTIONS_ONCE);

            nanoseconds_to_absolutetime(SWITCH_BUSY_MIN_MS * NSEC_PER_MSEC, &busy_min);
            if (call != NULL && !os_atomic_cmpxchg(&busy_call, NULL, call, release)) {
                thread_call_free(call);
            }
        }
        /* The process this slot watched has exited, so no departure or dispatch of its thread writes the slot now. */
        simple_lock(&busy_lock, LCK_GRP_NULL);
        os_atomic_store(&s->tid, 0, release);
        s->state = (struct switch_busy){0};
        s->pid = task_pid(get_threadtask(thread));
        os_atomic_store(&s->tid, thread->thread_id, release);
        simple_unlock(&busy_lock);
        os_atomic_store(&switch_sched_busy_watching, 1, relaxed);
        os_log(OS_LOG_DEFAULT, "SwitchBusy: watching %s[%d] main thread 0x%llx for stretches of %d ms or more", s->name,
               s->pid, thread_tid(thread), SWITCH_BUSY_MIN_MS);
        return;
    }
}

static void busy_wait_of(thread_t thread, struct switch_busy_wait *w)
{
    const task_t task = get_threadtask(thread);
    const struct vm_shared_region *sr = task->shared_region;
    const arm_saved_state_t *user = get_user_regs(thread);
    uint64_t value = 0;

    w->hint = (uint8_t)thread->block_hint;
    w->event = VM_KERNEL_UNSLIDE_OR_PERM(thread->wait_event);
    w->kernel =
        thread->continuation != NULL
            ? VM_KERNEL_UNSLIDE((uintptr_t)ptrauth_strip((void *)thread->continuation, ptrauth_key_function_pointer))
            : 0;
    if (user == NULL) {
        return;
    }
    w->pc_where = (uint8_t)switch_stall_user_where(
        (uint64_t)ptrauth_strip((void *)get_saved_state_pc(user), ptrauth_key_return_address),
        sr ? sr->sr_base_address : 0, sr ? sr->sr_size : 0, sr ? sr->sr_slide : 0, task->mach_header_vm_address,
        &value);
    w->pc = value;
    w->lr_where = (uint8_t)switch_stall_user_where(
        (uint64_t)ptrauth_strip((void *)get_saved_state_lr(user), ptrauth_key_return_address),
        sr ? sr->sr_base_address : 0, sr ? sr->sr_size : 0, sr ? sr->sr_slide : 0, task->mach_header_vm_address,
        &value);
    w->lr = value;
}

/* thread_dispatch, under the departing thread's lock: no lock, allocation or log here. */
void switch_sched_busy_departed(thread_t thread, uint64_t now)
{
    struct busy_slot *s = busy_slot_of(thread);
    struct switch_busy_wait wait = {0};
    struct switch_busy_episode done;
    bool waiting, idle;

    if (s == NULL) {
        return;
    }
    waiting = (thread->state & TH_WAIT) != 0;
    idle = waiting && !waitq_is_null(thread->waitq) && waitq_type(thread->waitq) == WQT_PORT_SET;
    if (waiting && !idle) {
        busy_wait_of(thread, &wait);
    }
    if (!switch_busy_departed(&s->state, now, recount_thread_time_mach(thread), waiting, idle, &wait, busy_min,
                              &done)) {
        return;
    }
    if (s->head - os_atomic_load(&s->tail, acquire) >= BUSY_RING) {
        os_atomic_store(&s->dropped, s->dropped + 1, relaxed);
        return;
    }
    s->ring[s->head % BUSY_RING] = done;
    os_atomic_store(&s->head, s->head + 1, release);
}

/* thread_invoke, under the incoming thread's lock: no lock, allocation or log here. */
void switch_sched_busy_dispatched(thread_t thread, uint64_t now)
{
    struct busy_slot *s = busy_slot_of(thread);
    const uint64_t made = thread->last_made_runnable_time;

    if (s != NULL) {
        switch_busy_dispatched(&s->state, now, made == THREAD_NOT_RUNNABLE ? 0 : made);
    }
}

/* The scheduler's maintenance tick (through switch_stall_tick). */
void switch_sched_busy_tick(void)
{
    thread_call_t call = os_atomic_load(&busy_call, acquire);

    if (call == NULL) {
        return;
    }
    for (unsigned int i = 0; i < sizeof(busy_slots) / sizeof(busy_slots[0]); i++) {
        if (os_atomic_load(&busy_slots[i].head, acquire) != os_atomic_load(&busy_slots[i].tail, relaxed)) {
            thread_call_enter(call);
            return;
        }
    }
}

static void busy_write(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    static char line[BUSY_LINE];
    mach_timebase_info_data_t tb;

    clock_timebase_info(&tb);
    for (unsigned int i = 0; i < sizeof(busy_slots) / sizeof(busy_slots[0]); i++) {
        struct busy_slot *s = &busy_slots[i];
        uint32_t tail = os_atomic_load(&s->tail, relaxed);
        int pid;
        uint64_t tid;

        simple_lock(&busy_lock, LCK_GRP_NULL);
        pid = s->pid;
        tid = os_atomic_load(&s->tid, relaxed);
        simple_unlock(&busy_lock);
        while (tail != os_atomic_load(&s->head, acquire)) {
            const struct switch_busy_episode e = s->ring[tail % BUSY_RING];
            uint64_t woke_ns = 0, len_ns = 0;
            int n;

            os_atomic_store(&s->tail, ++tail, release);
            absolutetime_to_nanoseconds(e.woke, &woke_ns);
            absolutetime_to_nanoseconds(e.end - e.woke, &len_ns);
            n = snprintf(line, sizeof(line),
                         "%s[%d] main thread 0x%llx busy %llu ms from its run loop's wake at %llu.%03llu s "
                         "(%u not logged): ",
                         s->name, pid, tid, len_ns / NSEC_PER_MSEC, woke_ns / NSEC_PER_SEC,
                         woke_ns % NSEC_PER_SEC / NSEC_PER_MSEC, os_atomic_load(&s->dropped, relaxed));
            if (n > 0 && (size_t)n < sizeof(line)) {
                switch_busy_format(line + n, sizeof(line) - (size_t)n, &e, tb.numer, tb.denom, BUSY_KINDS);
            }
            os_log(OS_LOG_DEFAULT, "SwitchBusy: %s", line);
        }
    }
}

#endif /* DEVELOPMENT || DEBUG */
