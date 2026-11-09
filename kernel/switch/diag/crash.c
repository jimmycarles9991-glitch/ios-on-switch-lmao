// SPDX-License-Identifier: GPL-2.0-only
/*
 * A process that crashed (debug kernels, diag/exit.c decides): its exception, its faulting thread's registers and its
 * frame chain, as kernel log lines. Early in boot there is no /private/var/cores, so these lines are all a crash
 * leaves. Addresses in the shared cache are unslid ("dsc", as `ipsw dyld a2s` takes them), the main executable's are
 * offsets ("main+").
 */

#include <switch/diag.h>

#include "stall_snapshot.h"

#include <arm/thread.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/arm/thread_status.h>
#include <vm/vm_shared_region.h>
#if XNU_12377_121_6
#include <vm/vm_shared_region_xnu.h>
#endif

#if DEVELOPMENT || DEBUG

void switch_diag_user_crash(task_t task, const char *name, int pid, int signal, int exception, int64_t code,
                            int64_t subcode)
{
    struct stall_line line = {.at = 0};

    printf("SwitchCrash: %s[%d] signal %d exception %d code 0x%llx subcode 0x%llx\n", name, pid, signal, exception,
           (uint64_t)code, (uint64_t)subcode);
    const arm_saved_state_t *state = task == current_task() ? get_user_regs(current_thread()) : NULL;
    if (state == NULL || !is_saved_state64(state)) {
        return;
    }
    const arm_saved_state64_t *regs = const_saved_state64(state);
    printf("SwitchCrash: pc 0x%llx lr 0x%llx sp 0x%llx fp 0x%llx far 0x%llx esr 0x%x cpsr 0x%x slide 0x%x\n", regs->pc,
           regs->lr, regs->sp, regs->fp, regs->far, (uint32_t)regs->esr, regs->cpsr,
           task->shared_region ? task->shared_region->sr_slide : 0);
    for (unsigned int i = 0; i < 29; i += 8) {
        line.at = 0;
        for (unsigned int r = i; r < i + 8 && r < 29; r++) {
            switch_stall_put(&line, " x%u 0x%llx", r, regs->x[r]);
        }
        printf("SwitchCrash:%s\n", line.text);
    }
    struct stall_snapshot frames = {.pc = regs->pc, .lr = regs->lr, .fp = regs->fp, .task = task};
    line.at = 0;
    switch_stall_put_user(&line, &frames);
    switch_stall_put_address(&line, task, "; far ", regs->far);
    printf("SwitchCrash:%s\n", line.text);
}

#endif
