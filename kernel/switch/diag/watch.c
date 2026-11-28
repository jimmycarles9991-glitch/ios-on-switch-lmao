// SPDX-License-Identifier: GPL-2.0-only
/*
 * A kernel path's one stall line (debug kernels, switch/stall.h switch_stall_watch_begin): when its thread is still in
 * it after a while and blocked, the wait it is in and whom that wait is for, as the UI stall lines name them
 * (stall_snapshot.h). One thread call per watch, armed at its start and cancelled at its end: nothing runs while the
 * path is quick.
 */

#include <switch/stall.h>

#include "stall_snapshot.h"

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <os/log.h>

#if DEVELOPMENT || DEBUG

struct switch_stall_watch {
    thread_t thread; /* +1 */
    thread_call_t call;
    uint64_t began;
    const char *what;
};

static void watch_fire(thread_call_param_t p0, __unused thread_call_param_t p1)
{
    struct switch_stall_watch *w = p0;
    struct stall_snapshot snap = {0};
    struct stall_line l = {.at = 0};
    struct stall_line chain = {.at = 0};
    bool blocked = false;
    uint64_t ns = 0;
    spl_t s = splsched();

    thread_lock(w->thread);
    if (!(w->thread->state & TH_TERMINATE) && !switch_stall_on_core(w->thread) &&
        switch_stall_off_core_waiting(w->thread->state)) {
        switch_stall_take(w->thread, &snap);
        switch_stall_take_rwlock(w->thread, &snap.wait);
        blocked = true;
    }
    thread_unlock(w->thread);
    splx(s);
    if (!blocked) {
        return;
    }
    switch_stall_take_owner_chain(&snap, STALL_MORE);
    absolutetime_to_nanoseconds(snap.taken - w->began, &ns);
    switch_stall_put(&l, "%s: thread 0x%llx blocked in it after %llu ms (pri %d):", w->what, snap.tid,
                     ns / NSEC_PER_MSEC, snap.wait.sched_pri);
    switch_stall_put_wait(&l, &snap);
    switch_stall_put_kernel(&l, "kernel", &snap.wait);
    os_log(OS_LOG_DEFAULT, "SwitchStall: %s", l.text);
    if (switch_stall_put_chain(&chain, &snap)) {
        os_log(OS_LOG_DEFAULT, "SwitchStall: chain: %s", chain.text);
    }
    switch_stall_release(&snap);
}

struct switch_stall_watch *switch_stall_watch_begin(thread_t thread, uint64_t ns, const char *what)
{
    struct switch_stall_watch *w = kalloc_type(struct switch_stall_watch, Z_WAITOK | Z_ZERO);
    uint64_t deadline = 0;

    if (w == NULL) {
        return NULL;
    }
    w->call = thread_call_allocate_with_options(watch_fire, w, THREAD_CALL_PRIORITY_USER, THREAD_CALL_OPTIONS_ONCE);
    if (w->call == NULL) {
        kfree_type(struct switch_stall_watch, w);
        return NULL;
    }
    thread_reference(thread);
    w->thread = thread;
    w->what = what;
    w->began = mach_absolute_time();
    nanoseconds_to_absolutetime(ns, &deadline);
    thread_call_enter_delayed(w->call, w->began + deadline);
    return w;
}

void switch_stall_watch_end(struct switch_stall_watch *w)
{
    if (w == NULL) {
        return;
    }
    thread_call_cancel_wait(w->call);
    thread_call_free(w->call);
    thread_deallocate(w->thread);
    kfree_type(struct switch_stall_watch, w);
}

#endif /* DEVELOPMENT || DEBUG */
