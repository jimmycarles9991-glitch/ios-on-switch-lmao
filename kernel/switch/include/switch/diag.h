// SPDX-License-Identifier: GPL-2.0-only
/*
 * What a stuck boot prints before its panic (kernel/switch/diag/threads.c; NativeSwitchBlock calls it by name), and
 * what a process that crashes leaves in the kernel log (diag/crash.c).
 */
#ifndef SWITCH_DIAG_H
#define SWITCH_DIAG_H

#include <stddef.h>
#include <stdint.h>
#include <switch/workloop_handoff.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct task;
struct vnode;

void switch_print_blocked_threads(unsigned max_threads);
void switch_print_thread(const void *thread, const char *who);
void switch_print_processors(const char *who);
void switch_sched_describe(char *buffer, size_t size, const void *thread);

/* vm_page_sleep: a thread waits for a busy page. The file read it waits for, if it is one (an absent page of a file's
 * object), is taken under the object's lock before the sleep; the wait is counted after it (diag/pagein.c). */
struct switch_page_wait {
    const struct vnode *file;
    uint64_t began;
};

/* NativeSwitchBlock's cryptex read cache (SwitchCryptex.cpp): its counts since the call before, as text after the
 * 10 s report's "SwitchSched cryptex cache (<ms> ms): " (debug kernels: switch_diag_cryptex_cache, below). */
typedef size_t (*switch_cryptex_cache_counts_t)(char *out, size_t size);

/*
 * proc_prepareexit: a process exits with status rv; one that crashed is logged (diag/exit.c, diag/crash.c).
 * The CPU monitor's violation, on the offending thread: its user registers and frames (diag/crash.c). spindump's
 * cpu_resource report has no stack on this kernel (no microstackshots), so these lines are the only ones.
 */
struct proc;
#if DEVELOPMENT || DEBUG
void switch_diag_proc_exit(struct proc *p, int rv);
void switch_diag_user_crash(struct task *task, const char *name, int pid, int signal, int exception, int64_t code,
                            int64_t subcode);
void switch_diag_cpu_hog(struct task *task, const char *name, int pid);
/* log_unnest_badness, after its rate limit: the current thread unnests [start, end) of its shared region (diag/unnest.c). */
void switch_diag_unnest(uint64_t start, uint64_t end);
/* vnode_pager_data_request, on the faulting thread after the pager's read: `bytes` asked of `vp`, `read` (mach) spent
 * in it: the whole read when the file system reads synchronously (a compressed file), its issue otherwise, the wait
 * then being a busy-page wait (diag/pagein.c); the 10 s scheduling report's lines of them. */
void switch_diag_pagein_report(char *line, size_t size, uint64_t window_ns);
void switch_diag_cryptex_cache(switch_cryptex_cache_counts_t counts);
#else
static inline void switch_diag_proc_exit(__unused struct proc *p, __unused int rv)
{}

static inline void switch_diag_cpu_hog(__unused struct task *task, __unused const char *name, __unused int pid)
{}

static inline void switch_diag_unnest(__unused uint64_t start, __unused uint64_t end)
{}
#endif

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_DIAG_WORKLOOP) && SWITCH_DIAG_WORKLOOP
/* filt_wltouch, after a successful update: a sync wait that returns at once on another wait's knote (diag/workloop.c). */
void switch_diag_wl_touched(uint32_t command, uint32_t sfflags, uint64_t workloop, uint64_t ident, const void *waiter);
/* iOS 27's workloop filter, once one of <switch/workloop_handoff.h>'s events happened: its line (diag/workloop.c). */
void switch_diag_wl_event(enum switch_wl_kind kind, struct switch_wl_event *event);
/* filt_wlwait_continue, under the workloop's lock: the owner and servicer the line names, by thread id. */
void switch_diag_wl_hold(struct switch_wl_event *event, const void *owner, const void *servicer);

static inline bool switch_diag_wl_unheld(const void *self, const void *owner, const void *servicer)
{
    return __builtin_expect(owner != self && servicer != self, 0);
}

/* The waiter an EV_DELETE of its knote wakes (a SYNC_WAIT not yet woken), else NULL. */
static inline const void *switch_diag_wl_waking(bool unwoken_wait, const void *waiter)
{
    return __builtin_expect(unwoken_wait && waiter != NULL, 0) ? waiter : NULL;
}
#else
static inline void switch_diag_wl_touched(__unused uint32_t command, __unused uint32_t sfflags,
                                          __unused uint64_t workloop, __unused uint64_t ident,
                                          __unused const void *waiter)
{}

static inline void switch_diag_wl_event(__unused enum switch_wl_kind kind, __unused struct switch_wl_event *event)
{}

static inline void switch_diag_wl_hold(__unused struct switch_wl_event *event, __unused const void *owner,
                                       __unused const void *servicer)
{}

static inline bool switch_diag_wl_unheld(__unused const void *self, __unused const void *owner,
                                         __unused const void *servicer)
{
    return false;
}

static inline const void *switch_diag_wl_waking(__unused bool unwoken_wait, __unused const void *waiter)
{
    return NULL;
}
#endif

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO
void switch_diag_pagein(struct vnode *vp, uint64_t bytes, uint64_t read);
struct switch_page_wait switch_diag_page_wait(void *pager, unsigned int absent);
void switch_diag_page_waited(const struct switch_page_wait *wait);
#else
static inline void switch_diag_pagein(__unused struct vnode *vp, __unused uint64_t bytes, __unused uint64_t read)
{}

static inline struct switch_page_wait switch_diag_page_wait(__unused void *pager, __unused unsigned int absent)
{
    return (struct switch_page_wait){NULL, 0};
}

static inline void switch_diag_page_waited(__unused const struct switch_page_wait *wait)
{}
#endif

__END_DECLS

#endif
