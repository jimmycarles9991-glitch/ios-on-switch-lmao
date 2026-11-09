// SPDX-License-Identifier: GPL-2.0-only
/*
 * A process that crashes, by Apple's own rule for a crash report (debug kernels): a signal that dumps core, a
 * code-signing kill, or an exit reason that asks for a report (abort_with_reason, EXC_GUARD, ...). Its exit reason,
 * then its fault (diag/crash.c).
 */

#include <switch/diag.h>

#include <libkern/libkern.h>
#include <sys/codesign.h>
#include <sys/proc_internal.h>
#include <sys/reason.h>
#include <sys/signalvar.h>
#include <sys/user.h>
#include <sys/wait.h>

#if DEVELOPMENT || DEBUG

void switch_diag_proc_exit(proc_t p, int rv)
{
    const os_reason_t reason = p->p_exit_reason;
    const uint64_t flags = reason != OS_REASON_NULL ? reason->osr_flags : 0;

    if ((flags & OS_REASON_FLAG_NO_CRASH_REPORT) ||
        !(hassigprop(WTERMSIG(rv), SA_CORE) || (proc_getcsflags(p) & CS_KILLED) ||
          (flags & OS_REASON_FLAG_GENERATE_CRASH_REPORT))) {
        return;
    }
    if (reason != OS_REASON_NULL) {
        const char *desc = exit_reason_get_string_desc(reason);
        printf("SwitchCrash: %s[%d] exit reason namespace %u code 0x%llx%s%s\n", p->p_name, proc_getpid(p),
               reason->osr_namespace, reason->osr_code, desc ? ": " : "", desc ? desc : "");
    }
    const struct uthread *ut = p == current_proc() ? current_uthread() : NULL;
    switch_diag_user_crash(proc_task(p), p->p_name, proc_getpid(p), WTERMSIG(rv), ut ? ut->uu_exception : 0,
                           ut ? ut->uu_code : 0, ut ? ut->uu_subcode : 0);
}

#endif
