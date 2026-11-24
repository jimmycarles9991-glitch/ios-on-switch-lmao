// SPDX-License-Identifier: GPL-2.0-only
/*
 * The workloop hand-off probe (diag/workloop.c, debug kernels; hooked in iOS 27's kern_event.c only). Each kind is an
 * event dispatch_sync's contract with the kernel allows only for a queue retargeted with a sync in flight
 * (libdispatch's _dispatch_event_loop_cancel_waiter, which libdispatch itself reports as a client bug), or never:
 *   wait-unheld  a NOTE_WL_SYNC_WAIT woken, not interrupted, whose thread as it returns is neither the workloop's owner
 *                nor its bound servicer;
 *   drop-wake    an EV_DELETE of a SYNC_WAIT knote not yet woken, which wakes its waiter (filt_wlupdate, FILT_WLDROP);
 *   stale-wake   a SYNC_WAIT that returns at once on a knote already woken that no thread waits on, its thread neither
 *                owner nor servicer (a wake whose wait's END was lost);
 *   end-enoent   an EV_DELETE with NOTE_WL_END_OWNERSHIP that finds no knote;
 *   owner-dead   a NOTE_WL_DISCOVER_OWNER whose dq_state names no thread of the task (EOWNERDEAD).
 * A process gets a line for each of the first SWITCH_WL_FIRST events of a kind, then at each power of two; every line
 * carries the running count.
 */
#ifndef SWITCH_WORKLOOP_HANDOFF_H
#define SWITCH_WORKLOOP_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_WL_FIRST 8u

enum switch_wl_kind {
    SWITCH_WL_WAIT_UNHELD,
    SWITCH_WL_DROP_WAKE,
    SWITCH_WL_STALE_WAKE,
    SWITCH_WL_END_ENOENT,
    SWITCH_WL_OWNER_DEAD,
    SWITCH_WL_KINDS,
};

/* What a hook saw. The thread pointers are compared with the current thread and with each other, never followed; a
 * thread id is read only where the hook holds what keeps that thread alive: owner and servicer under the workloop's
 * lock (`held`), the waiter through its knote's reference. */
struct switch_wl_event {
    uint64_t workloop, ident, dq_state;
    uint32_t fflags;
    uint16_t flags;
    bool held;
    const void *owner, *servicer, *waiter;
    uint64_t owner_tid, servicer_tid, waiter_tid;
};

static inline const char *switch_wl_kind_name(enum switch_wl_kind kind)
{
    static const char *const names[SWITCH_WL_KINDS] = {"wait-unheld", "drop-wake", "stale-wake", "end-enoent",
                                                       "owner-dead"};
    return (unsigned int)kind < SWITCH_WL_KINDS ? names[kind] : "?";
}

/* Whether the n-th event of a kind in a process (from 1) gets a line. */
static inline bool switch_wl_counted(uint32_t n)
{
    return n <= SWITCH_WL_FIRST || (n & (n - 1)) == 0;
}

#endif
