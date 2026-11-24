// SPDX-License-Identifier: GPL-2.0-only
/*
 * A stall's wait chain past its first owner (stall_snapshot.h), debug kernels only: the owners each next one waits for,
 * and for a kernel rwlock the thread holding it for writing, as stackshot names them ("krwlock ... owned by thread").
 * Only the lock's 32-bit owner ctid is read under the waiter's thread lock; the writer thread is resolved on the
 * thread call under tasks_threads_lock so no unreferenced thread_t is ever dereferenced at splsched().
 */

#include <switch/xnu.h>

#include "stall.h"
#include "stall_snapshot.h"

#include <kern/lock_rw.h>
#include <kern/locks.h>
#include <kern/processor.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <stddef.h>

#if DEVELOPMENT || DEBUG

#if XNU_12377_121_6
_Static_assert(offsetof(lck_rw_old_t, lck_rw_owner) == offsetof(lck_rw_new_t, lck_rw_owner),
               "both xnu-12377 rwlock layouts keep lck_rw_owner at the same offset");
#define RW_EVENT_ALLOW_TYPED true
#else
#define RW_EVENT_ALLOW_TYPED false
#endif

extern void proc_name(int pid, char *buf, int size);

/* Under the waiter's lock, so the lock stays alive (its wakeup needs that lock): the exclusive owner's ctid, or 0. */
uint32_t switch_stall_rw_writer(thread_t t)
{
    const volatile lck_rw_t *lck = (const volatile lck_rw_t *)switch_stall_rwlock_of(
        (uint64_t)t->wait_event, (unsigned int)t->block_hint, RW_EVENT_ALLOW_TYPED);

    return lck->lck_rw_owner;
}

/* A thread by its ctid, +1 (THREAD_NULL: gone). Every thread on `threads` is alive and pre-ctid_table_remove. */
static thread_t thread_by_ctid(uint32_t ctid)
{
    thread_t t = THREAD_NULL, found = THREAD_NULL;

    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&threads, t, thread_t, threads)
    {
        if (t->ctid == ctid) {
            thread_reference(t);
            found = t;
            break;
        }
    }
    lck_mtx_unlock(&tasks_threads_lock);
    return found;
}

/* " <label> kernel rwlock is held for writing by <process>[pid] thread 0x... <name>: <its wait>" */
static bool put_writer(struct stall_line *l, const char *label, const struct stall_wait *w)
{
    char process[STALL_NAME], name[64]; /* thread_get_thread_name copies MAXTHREADNAMESIZE bytes */
    struct stall_wait held = {0};
    bool on_core = false, taken;
    thread_t t;

    if (!w->rwlock) {
        return false;
    }
    if (w->rw_writer_ctid == 0) {
        switch_stall_put(l, " %s kernel rwlock has no exclusive owner (readers hold it, or a writer waits);", label);
        return true;
    }
    t = thread_by_ctid(w->rw_writer_ctid);
    if (t == THREAD_NULL) {
        switch_stall_put(l, " %s kernel rwlock is held for writing by ctid 0x%x (exited);", label, w->rw_writer_ctid);
        return true;
    }
    switch_task_name(get_threadtask(t), process, sizeof(process));
    thread_get_thread_name(t, name);
    taken = switch_stall_take_thread(t, &on_core, &held, NULL, false);
    switch_stall_put(l, " %s kernel rwlock is held for writing by %s[%d] thread 0x%llx %s", label, process,
                     task_pid(get_threadtask(t)), thread_tid(t), name[0] ? name : "-");
    if (!taken) {
        switch_stall_put(l, ": terminating;");
    } else if (on_core) {
        switch_stall_put(l, ": on core (pri %d);", held.sched_pri);
    } else if (switch_stall_off_core_waiting(held.state)) {
        switch_stall_put(l, ": blocked (pri %d):", held.sched_pri);
        switch_stall_put_blocked(l, &held, false);
        switch_stall_put_kernel(l, "its kernel", &held);
    } else {
        switch_stall_put(l, ": on a run queue (pri %d);", held.sched_pri);
    }
    thread_deallocate(t);
    return true;
}

bool switch_stall_put_chain(struct stall_line *l, const struct stall_snapshot *ss)
{
    char owner[32];
    bool any = put_writer(l, "its", &ss->wait);

    if (ss->owner_taken) {
        any |= put_writer(l, "the first owner's", &ss->owner_wait);
    }
    for (unsigned int i = 0; i < ss->nmore; i++) {
        const struct stall_hop *hop = &ss->more[i];

        any = true;
        if (!switch_stall_owner_known(&hop->owner)) {
            switch_stall_put(l, " owner %u not read (a lock on the way was held);", i + 2);
            break;
        }
        proc_name(hop->owner.pid, owner, sizeof(owner));
        switch_stall_put(l, " owner %u: %s[%d]", i + 2, owner, hop->owner.pid);
        if (hop->owner.tid != 0) {
            switch_stall_put(l, " thread 0x%llx", hop->owner.tid);
        }
        switch_stall_put(l, " (%s%s);", hop->owner.via, hop->special_reply ? ", its special reply port" : "");
        if (hop->taken && hop->on_core) {
            switch_stall_put(l, " it is on core (pri %d);", hop->wait.sched_pri);
        } else if (hop->taken && switch_stall_off_core_waiting(hop->wait.state)) {
            switch_stall_put(l, " it is blocked (pri %d):", hop->wait.sched_pri);
            switch_stall_put_blocked(l, &hop->wait, false);
            switch_stall_put_kernel(l, "its kernel", &hop->wait);
            (void)put_writer(l, "that", &hop->wait);
        } else if (hop->taken) {
            switch_stall_put(l, " it is on a run queue (pri %d);", hop->wait.sched_pri);
        }
    }
    return any;
}

#endif /* DEVELOPMENT || DEBUG */
