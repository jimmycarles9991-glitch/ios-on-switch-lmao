// SPDX-License-Identifier: GPL-2.0-only
/*
 * A dispatch_sync waiter that leaves its wait without a handoff (debug kernels). libdispatch's waiter registers
 * NOTE_WL_SYNC_WAIT on its own thread port name as the knote ident and trusts a return without error as ownership of
 * the workloop; it never reads the queue state again. A knote that already carries NOTE_WL_SYNC_WAKE returns at once.
 * That is a handoff that raced ahead of the wait only while no thread has waited on the knote yet (kn_thread NULL).
 * With kn_thread set, the wake belonged to an earlier wait whose knote was never deleted (or to another thread with
 * the same ident), and this wait returns with the queue still owned elsewhere: locationd's stale sync waiter on
 * iOS 27, where the drainer later reaches the waiter's dead dsc. Each such return is logged, a few per process.
 */

#include <switch/diag.h>

#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <sys/event_private.h>
#include <sys/proc.h>

#if DEVELOPMENT || DEBUG

#define WL_PROCESSES     32
#define WL_LINES_PER_PID 4

static struct {
    int pid;
    uint32_t lines;
} wl_seen[WL_PROCESSES];

static bool wl_may_log(int pid)
{
    for (unsigned int i = 0; i < WL_PROCESSES; i++) {
        int owner = 0;
        if (!os_atomic_cmpxchgv(&wl_seen[i].pid, 0, pid, &owner, relaxed) && owner != pid) {
            continue;
        }
        return os_atomic_inc_orig(&wl_seen[i].lines, relaxed) < WL_LINES_PER_PID;
    }
    return false;
}

void switch_diag_wl_touched(uint32_t command, uint32_t sfflags, uint64_t workloop, uint64_t ident, const void *waiter)
{
    if (command != NOTE_WL_SYNC_WAIT || !(sfflags & NOTE_WL_SYNC_WAKE) || waiter == NULL) {
        return;
    }
    int pid = proc_selfpid();
    if (!wl_may_log(pid)) {
        return;
    }
    thread_t self = current_thread();
    char name[32];
    proc_selfname(name, sizeof(name));
    printf("SwitchWL: %s[%d] thread 0x%llx: sync wait on workloop 0x%llx (ident 0x%llx) returned at once on a knote "
           "already woken for %s thread 0x%llx (sfflags 0x%x)\n",
           name, pid, thread_tid(self), workloop, ident, waiter == self ? "this" : "another",
           thread_tid((thread_t)(uintptr_t)waiter), sfflags);
}

#endif
