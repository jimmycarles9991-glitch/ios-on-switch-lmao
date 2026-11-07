// SPDX-License-Identifier: GPL-2.0-only
/*
 * A stalled thread's snapshot and its text (stall_snapshot.h). Debug kernels only.
 *
 * Locking: a waiter is woken under its wait queue's lock and then its thread lock, so with the thread lock held every
 * wait queue, port and turnstile lock is only try-locked; a lock that is held is reported as such. The objects
 * followed are alive while the thread lock is held: the thread stays enqueued on its wait queue (its wakeup needs the
 * thread lock), so its own turnstile and the port it belongs to cannot be torn down; inheritors and a special reply
 * port's sync inheritor port carry references while their holder is locked. Ports further up a chain carry none and
 * are never dereferenced. A thread at the end of the chain is referenced under its turnstile's lock (the turnstile
 * holds one on its inheritor) and looked at only after the waiter's lock is dropped: two thread locks never nest.
 */

#include <switch/xnu.h>

#include "stall_snapshot.h"

#include <arm/machine_routines.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_space.h>
#include <kern/block_hint.h>
#include <kern/cpu_number.h>
#include <kern/processor.h>
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/turnstile.h>
#include <kern/waitq.h>
#include <mach/vm_param.h>
#include <ptrauth.h>
#include <stdarg.h>
#include <string.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_shared_region.h>
#if XNU_12377_121_6
#include <vm/vm_kern_xnu.h>
#include <vm/vm_shared_region_xnu.h>
#endif

#include "stall.h"

#if DEVELOPMENT || DEBUG

#define STALL_HOPS 8

extern turnstile_type_t switch_turnstile_type(struct turnstile *turnstile);
extern int switch_workq_pid(void *workq);
extern void proc_name(int pid, char *buf, int size);
extern void bsd_getthreadname(void *uth, char *buffer);
extern int vsnprintf(char *, size_t, const char *, va_list);

static bool special_reply_port(ipc_port_t port)
{
#if XNU_12377_121_6
    return ip_is_special_reply_port(port);
#else
    return port->ip_specialreply;
#endif
}

static void port_receiver(ipc_port_t port, const char *via, struct stall_snapshot *ss)
{
    ipc_space_t space;

    if (!ip_mq_lock_try(port)) {
        ss->locked = true;
        return;
    }
    if (ip_active(port) && (space = ip_get_receiver(port)) != IS_NULL && space->is_task != TASK_NULL) {
        ss->owner.pid = task_pid(space->is_task);
        ss->owner.via = via;
    }
    ip_mq_unlock(port);
}

/* The special reply port a sync request waits on: who received the request, else where it is queued. */
static void special_reply(ipc_port_t port, struct stall_snapshot *ss)
{
    ipc_port_t dest = IP_NULL;

    if (!ip_mq_lock_try(port)) {
        ss->locked = true;
        return;
    }
    if (ip_active(port) && special_reply_port(port)) {
        ss->special_reply = true;
        if (ipc_special_reply_get_pid_locked(port) != 0) {
            ss->owner.pid = ipc_special_reply_get_pid_locked(port);
            ss->owner.via = "received the request";
        } else if (port->ip_sync_link_state == PORT_SYNC_LINK_PORT) {
            dest = port->ip_sync_inheritor_port;
        }
    }
    if (dest != IP_NULL) {
        port_receiver(dest, "receives the port the request is queued at", ss);
    }
    ip_mq_unlock(port);
}

/* The end of the turnstile's inheritor chain: the thread or workqueue that must run for the waiter to wake. */
static void inheritor(struct turnstile *ts, struct stall_snapshot *ss)
{
    struct turnstile *held = TURNSTILE_NULL;

    for (unsigned int hop = 0; ts != TURNSTILE_NULL && hop < STALL_HOPS; hop++) {
        struct turnstile *next = TURNSTILE_NULL;
        turnstile_update_flags_t flags;
        void *owner;

        if (!waitq_lock_try(&ts->ts_waitq)) {
            ss->locked = true;
            break;
        }
        if (held != TURNSTILE_NULL) {
            waitq_unlock(&held->ts_waitq);
        }
        held = ts;
        flags = ts->ts_inheritor_flags;
        owner = ts->ts_inheritor;
        if (owner == NULL) {
            break;
        }
        if (flags & TURNSTILE_INHERITOR_THREAD) {
            thread_t thread = (thread_t)owner;

            ss->owner.pid = task_pid(get_threadtask(thread));
            ss->owner.tid = thread_tid(thread);
            ss->owner.via = "turnstile";
            thread_reference(thread);
            ss->owner_thread = thread;
            break;
        }
        if (flags & TURNSTILE_INHERITOR_WORKQ) {
            ss->owner.pid = switch_workq_pid(owner);
            ss->owner.tid = 0;
            ss->owner.via = "workqueue";
            break;
        }
        if (flags & TURNSTILE_INHERITOR_TURNSTILE) {
            next = (struct turnstile *)owner;
        }
        ts = next;
    }
    if (held != TURNSTILE_NULL) {
        waitq_unlock(&held->ts_waitq);
    }
}

static void owner_of(thread_t t, struct stall_snapshot *ss)
{
    waitq_t wq = t->waitq;
    struct turnstile *ts;

    if (waitq_is_null(wq) || waitq_type(wq) == WQT_PORT_SET) {
        return;
    }
    ts = thread_get_waiting_turnstile(t);
    inheritor(ts, ss);
    if (ss->owner.pid != 0 && ss->owner.tid != 0) {
        return;
    }
    if (waitq_type(wq) == WQT_PORT) {
        special_reply(ip_from_waitq(wq.wq_q), ss);
    } else if (ts != TURNSTILE_NULL && switch_turnstile_type(ts) == TURNSTILE_SYNC_IPC && ts->ts_proprietor != 0) {
        ipc_port_t port = (ipc_port_t)ts->ts_proprietor;

        if (t->block_hint == kThreadWaitPortSend) {
            port_receiver(port, "receives the port the send is blocked on", ss);
        } else {
            special_reply(port, ss);
        }
    }
}

bool switch_stall_on_core(thread_t t)
{
    return (t->state & TH_RUN) && thread_get_runq(t) == PROCESSOR_NULL;
}

bool switch_stall_off_core_waiting(int state)
{
    return (state & (TH_WAIT | TH_RUN)) == TH_WAIT;
}

/* Under the thread's lock, off core: its wait and the kernel half of its stack (a running thread's saved state is stale). */
static void take_wait(thread_t t, struct stall_wait *w)
{
    w->state = t->state;
    w->sched_pri = t->sched_pri;
    w->hint = t->block_hint;
    w->event = VM_KERNEL_UNSLIDE_OR_PERM(t->wait_event);
    if (!waitq_is_null(t->waitq)) {
        w->wq_type = waitq_type(t->waitq);
        w->wq_object = VM_KERNEL_UNSLIDE_OR_PERM(t->waitq.wq_q);
    }
    if (switch_stall_on_core(t)) {
        return;
    }
    if (t->continuation != NULL) {
        w->continuation =
            VM_KERNEL_UNSLIDE((uintptr_t)ptrauth_strip((void *)t->continuation, ptrauth_key_function_pointer));
    } else if (t->kernel_stack != 0) {
        const vm_offset_t base = t->kernel_stack, top = base + kernel_stack_size;
        const arm_kernel_saved_state_t *k = &thread_get_kernel_state(t)->machine.ss;
        uintptr_t fp = k->fp;

        w->kframes[w->nkframes++] =
            VM_KERNEL_UNSLIDE((uintptr_t)ptrauth_strip((void *)k->lr, ptrauth_key_return_address));
        while (w->nkframes < STALL_KFRAMES && (fp & 7) == 0 && fp >= base && fp + 16 <= top) {
            const uintptr_t *frame = (const uintptr_t *)fp;

            w->kframes[w->nkframes++] =
                VM_KERNEL_UNSLIDE((uintptr_t)ptrauth_strip((void *)frame[1], ptrauth_key_return_address));
            if (frame[0] <= fp) {
                break;
            }
            fp = frame[0];
        }
    }
}

void switch_stall_take(thread_t t, struct stall_snapshot *ss)
{
    const arm_saved_state_t *user = get_user_regs(t);

    ss->taken = mach_absolute_time();
    ss->tid = thread_tid(t);
    take_wait(t, &ss->wait);
    if (user != NULL && !switch_stall_on_core(t)) {
        ss->pc = get_saved_state_pc(user);
        ss->lr = get_saved_state_lr(user);
        ss->fp = get_saved_state_fp(user);
    }
    if (switch_stall_off_core_waiting(ss->wait.state)) {
        owner_of(t, ss);
    }
    ss->task = get_threadtask(t);
    task_reference(ss->task);
}

void switch_stall_take_owner(struct stall_snapshot *ss)
{
    thread_t owner = ss->owner_thread;
    spl_t s;

    if (owner == THREAD_NULL) {
        return;
    }
    s = splsched();
    thread_lock(owner);
    if (!(owner->state & TH_TERMINATE)) {
        ss->owner_on_core = switch_stall_on_core(owner);
        take_wait(owner, &ss->owner_wait);
        ss->owner_taken = true;
    }
    thread_unlock(owner);
    splx(s);
}

void switch_stall_release(struct stall_snapshot *ss)
{
    if (ss->owner_thread != THREAD_NULL) {
        thread_deallocate(ss->owner_thread);
        ss->owner_thread = THREAD_NULL;
    }
    if (ss->task != TASK_NULL) {
        task_deallocate(ss->task);
        ss->task = TASK_NULL;
    }
}

/* On each other core, in its cross-call interrupt: the thread it interrupted, which cannot exit meanwhile. */
static void census_one(void *arg)
{
    const int n = cpu_number();
    struct stall_cpu *cpu;
    thread_t t = current_thread();
    char thread_name[64]; /* bsd_getthreadname copies MAXTHREADNAMESIZE bytes */

    if (n < 0 || n >= STALL_CPUS) {
        return;
    }
    cpu = (struct stall_cpu *)arg + n;
    cpu->seen = true;
    cpu->idle = t == current_processor()->idle_thread;
    if (cpu->idle) {
        return;
    }
    cpu->tid = thread_tid(t);
    cpu->sched_pri = t->sched_pri;
    cpu->mode = t->sched_mode;
    cpu->pid = task_pid(get_threadtask(t));
    switch_task_name(get_threadtask(t), cpu->process, sizeof(cpu->process));
    bsd_getthreadname(get_bsdthread_info(t), thread_name);
    strlcpy(cpu->thread, thread_name, sizeof(cpu->thread));
}

void switch_stall_census(struct stall_cpu cpus[STALL_CPUS])
{
    memset(cpus, 0, sizeof(struct stall_cpu) * STALL_CPUS);
    cpu_broadcast_xcall_simple(false, census_one, cpus);
}

void switch_stall_put(struct stall_line *l, const char *fmt, ...)
{
    va_list ap;

    if (l->at >= sizeof(l->text)) {
        return;
    }
    va_start(ap, fmt);
    l->at += (size_t)vsnprintf(l->text + l->at, sizeof(l->text) - l->at, fmt, ap);
    va_end(ap);
}

static const char *wq_name(int type)
{
    switch (type) {
    case WQT_QUEUE:
        return "wait queue";
    case WQT_TURNSTILE:
        return "turnstile";
    case WQT_PORT:
        return "port";
    case WQT_SELECT:
        return "select";
    case WQT_PORT_SET:
        return "port set";
    case WQT_SELECT_SET:
        return "select set";
    default:
        return "no wait queue";
    }
}

static const char *mode_name(int mode)
{
    return mode == TH_MODE_REALTIME ? "RT" : mode == TH_MODE_FIXED ? "FX" : mode == TH_MODE_TIMESHARE ? "TS" : "-";
}

void switch_stall_put_kernel(struct stall_line *l, const char *label, const struct stall_wait *w)
{
    if (w->continuation != 0) {
        switch_stall_put(l, " %s continuation 0x%lx;", label, w->continuation);
    } else if (w->nkframes != 0) {
        switch_stall_put(l, " %s", label);
        for (unsigned int i = 0; i < w->nkframes; i++) {
            switch_stall_put(l, " 0x%lx", w->kframes[i]);
        }
        switch_stall_put(l, ";");
    }
}

static void put_blocked(struct stall_line *l, const struct stall_wait *w, bool special)
{
    switch_stall_put(l, " %s%s on %s 0x%lx, event 0x%llx;", switch_stall_wait_name(w->hint),
                     special ? " (special reply port)" : "", wq_name(w->wq_type), w->wq_object, w->event);
}

void switch_stall_put_wait(struct stall_line *l, const struct stall_snapshot *ss)
{
    char owner[32] = "";

    if (!switch_stall_off_core_waiting(ss->wait.state)) {
        switch_stall_put(l, " %s;", (ss->wait.state & TH_SUSP) ? "suspended" : "on a run queue");
        return;
    }
    put_blocked(l, &ss->wait, ss->special_reply);
    if (ss->owner.pid != 0 || ss->owner.tid != 0) {
        proc_name(ss->owner.pid, owner, sizeof(owner));
        switch_stall_put(l, " waits for %s[%d]", owner, ss->owner.pid);
        if (ss->owner.tid != 0) {
            switch_stall_put(l, " thread 0x%llx", ss->owner.tid);
        }
        switch_stall_put(l, " (%s);", ss->owner.via);
        if (ss->owner_taken && ss->owner_on_core) {
            switch_stall_put(l, " it is on core (pri %d);", ss->owner_wait.sched_pri);
        } else if (ss->owner_taken && switch_stall_off_core_waiting(ss->owner_wait.state)) {
            switch_stall_put(l, " it is blocked (pri %d):", ss->owner_wait.sched_pri);
            put_blocked(l, &ss->owner_wait, false);
            switch_stall_put_kernel(l, "its kernel", &ss->owner_wait);
        } else if (ss->owner_taken) {
            switch_stall_put(l, " it is on a run queue (pri %d);", ss->owner_wait.sched_pri);
        }
    } else if (ss->locked) {
        switch_stall_put(l, " owner not read (a lock on the way was held);");
    }
}

static void put_address(struct stall_line *l, task_t task, const char *label, uint64_t pc)
{
    const struct vm_shared_region *sr = task->shared_region;
    uint64_t value;

    pc = (uint64_t)ptrauth_strip((void *)pc, ptrauth_key_return_address);
    switch (switch_stall_user_where(pc, sr ? sr->sr_base_address : 0, sr ? sr->sr_size : 0, sr ? sr->sr_slide : 0,
                                    task->mach_header_vm_address, &value)) {
    case SWITCH_STALL_DSC:
        switch_stall_put(l, "%sdsc 0x%llx", label, value);
        break;
    case SWITCH_STALL_MAIN:
        switch_stall_put(l, "%smain+0x%llx", label, value);
        break;
    case SWITCH_STALL_RAW:
    default:
        switch_stall_put(l, "%s0x%llx", label, value);
        break;
    }
}

void switch_stall_put_user(struct stall_line *l, const struct stall_snapshot *ss)
{
    vm_map_t map;
    uint64_t fp = ss->fp;

    if (ss->pc == 0) {
        return;
    }
    put_address(l, ss->task, " user pc ", ss->pc);
    put_address(l, ss->task, " lr ", ss->lr);
    map = get_task_map_reference(ss->task);
    if (map == VM_MAP_NULL) {
        return;
    }
    switch_stall_put(l, ", frames");
    for (unsigned int i = 0; i < STALL_UFRAMES && fp != 0 && (fp & 7) == 0; i++) {
        uint64_t frame[2];

        if (copyinmap(map, fp, frame, sizeof(frame)) != KERN_SUCCESS || frame[1] == 0) {
            break;
        }
        put_address(l, ss->task, " ", frame[1]);
        if (frame[0] <= fp) {
            break;
        }
        fp = frame[0];
    }
    vm_map_deallocate(map);
}

void switch_stall_put_census(struct stall_line *l, const struct stall_cpu cpus[STALL_CPUS])
{
    switch_stall_put(l, "; the other cores run");
    for (unsigned int i = 0; i < STALL_CPUS; i++) {
        const struct stall_cpu *c = &cpus[i];

        if (!c->seen) {
            continue;
        }
        if (c->idle) {
            switch_stall_put(l, " cpu%u idle;", i);
            continue;
        }
        switch_stall_put(l, " cpu%u %s[%d] 0x%llx %s %s %d;", i, c->process, c->pid, c->tid,
                         c->thread[0] ? c->thread : "-", mode_name(c->mode), c->sched_pri);
    }
}

#endif /* DEVELOPMENT || DEBUG */
