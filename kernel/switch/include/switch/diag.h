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

/* proc_prepareexit: a process exits with status rv; one that crashed is logged (diag/exit.c, diag/crash.c). */
struct proc;
#if DEVELOPMENT || DEBUG
void switch_diag_proc_exit(struct proc *p, int rv);
void switch_diag_user_crash(struct task *task, const char *name, int pid, int signal, int exception, int64_t code,
                            int64_t subcode);
#else
static inline void switch_diag_proc_exit(__unused struct proc *p, __unused int rv)
{}
#endif

__END_DECLS

#endif
