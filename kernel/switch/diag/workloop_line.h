// SPDX-License-Identifier: GPL-2.0-only
/*
 * The SwitchWL handoff line (workloop.c), plain C so kernel/tests formats it on the host. The including file declares
 * snprintf.
 */
#ifndef SWITCH_DIAG_WORKLOOP_LINE_H
#define SWITCH_DIAG_WORKLOOP_LINE_H

#include <stddef.h>
#include <switch/workloop_handoff.h>

/* A thread the hook only compared reads as self, waiter, other or none; one it read under a reference, as its id. */
static inline int switch_wl_who(char *out, size_t size, const void *thread, uint64_t tid, bool read, const void *self,
                                const void *waiter)
{
    if (thread == NULL) {
        return snprintf(out, size, "none");
    }
    if (thread == self) {
        return snprintf(out, size, "self");
    }
    if (thread == waiter) {
        return snprintf(out, size, "waiter");
    }
    return read ? snprintf(out, size, "0x%llx", (unsigned long long)tid) : snprintf(out, size, "other");
}

static inline int switch_wl_format(char *out, size_t size, enum switch_wl_kind kind, uint32_t n, const char *process,
                                   int pid, uint64_t thread, const void *self, const struct switch_wl_event *e)
{
    char owner[24], servicer[24], waiter[32] = "";
    switch_wl_who(owner, sizeof(owner), e->owner, e->owner_tid, e->held, self, e->waiter);
    switch_wl_who(servicer, sizeof(servicer), e->servicer, e->servicer_tid, e->held, self, e->waiter);
    if (e->waiter != NULL) {
        snprintf(waiter, sizeof(waiter), " waiter 0x%llx", (unsigned long long)e->waiter_tid);
    }
    return snprintf(out, size,
                    "SwitchWL handoff %s #%u: %s[%d] thread 0x%llx workloop 0x%llx ident 0x%llx dq_state 0x%llx "
                    "fflags 0x%x flags 0x%x owner %s servicer %s%s",
                    switch_wl_kind_name(kind), n, process, pid, (unsigned long long)thread,
                    (unsigned long long)e->workloop, (unsigned long long)e->ident, (unsigned long long)e->dq_state,
                    e->fflags, (unsigned int)e->flags, owner, servicer, waiter);
}

#endif
