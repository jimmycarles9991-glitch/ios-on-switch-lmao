// SPDX-License-Identifier: GPL-2.0-only
/*
 * What a stuck boot prints before its panic (kernel/switch/diag/threads.c; NativeSwitchBlock calls it by name), and
 * what a process that crashes leaves in the kernel log (diag/crash.c).
 */
#ifndef SWITCH_DIAG_H
#define SWITCH_DIAG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct task;

void switch_print_blocked_threads(unsigned max_threads);
void switch_print_thread(const void *thread, const char *who);
void switch_print_processors(const char *who);
void switch_sched_describe(char *buffer, size_t size, const void *thread);

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
/* filt_wltouch, after a successful update: a sync wait that returns at once on another wait's knote (diag/workloop.c). */
void switch_diag_wl_touched(uint32_t command, uint32_t sfflags, uint64_t workloop, uint64_t ident, const void *waiter);
#else
static inline void switch_diag_proc_exit(__unused struct proc *p, __unused int rv)
{}

static inline void switch_diag_cpu_hog(__unused struct task *task, __unused const char *name, __unused int pid)
{}

static inline void switch_diag_wl_touched(__unused uint32_t command, __unused uint32_t sfflags,
                                          __unused uint64_t workloop, __unused uint64_t ident,
                                          __unused const void *waiter)
{}
#endif

__END_DECLS

#endif
