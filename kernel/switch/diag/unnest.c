// SPDX-License-Identifier: GPL-2.0-only
/*
 * A shared-region unnest XNU logs (log_unnest_badness, debug kernels): who asked for it. XNU's line names the process
 * and the range; this one adds the user code that trapped in (its PC and LR, as dyld shared cache or main-image
 * offsets), the call it made (x16: a BSD system call above 0, a Mach trap below), and the kernel path from the unnest up,
 * unslid, for atos. It rides XNU's own rate limit.
 */

#include <kern/misc_protos.h>
#include <switch/diag.h>
#include <switch/xnu.h>

#include "../sched/busy.h"
#include "stall.h"

#include <arm/thread.h>
#include <kern/backtrace.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <ptrauth.h>
#include <vm/vm_shared_region.h>
#if XNU_12377_121_6
#include <vm/vm_shared_region_xnu.h>
#endif

#if DEVELOPMENT || DEBUG

#define UNNEST_FRAMES 8

static void unnest_where(char *out, size_t size, uint64_t address)
{
    const task_t task = current_task();
    const struct vm_shared_region *sr = task->shared_region;
    uint64_t value = 0;
    const uint8_t where = (uint8_t)switch_stall_user_where(
        (uint64_t)ptrauth_strip((void *)address, ptrauth_key_return_address), sr ? sr->sr_base_address : 0,
        sr ? sr->sr_size : 0, sr ? sr->sr_slide : 0, task->mach_header_vm_address, &value);

    switch_busy_where(out, size, where, value);
}

void switch_diag_unnest(uint64_t start, uint64_t end)
{
    const arm_saved_state_t *user = get_user_regs(current_thread());
    uintptr_t frames[UNNEST_FRAMES] = {0};
    const unsigned int n = backtrace(frames, UNNEST_FRAMES, NULL, NULL);
    char name[32], pc[32] = "-", lr[32] = "-", path[UNNEST_FRAMES * 20] = "";
    long long call = 0;
    size_t at = 0;

    if (user != NULL) {
        unnest_where(pc, sizeof(pc), get_saved_state_pc(user));
        unnest_where(lr, sizeof(lr), get_saved_state_lr(user));
        call = (long long)get_saved_state_reg(user, 16);
    }
    /* frames[0] is in this function */
    for (unsigned int i = 1; i < n; i++) {
        at = switch_busy_put(
            sizeof(path), at,
            snprintf(path + at, sizeof(path) - at, " 0x%lx", (unsigned long)VM_KERNEL_UNSLIDE(frames[i])));
    }
    switch_task_name(current_task(), name, sizeof(name));
    os_log(OS_LOG_DEFAULT, "SwitchUnnest: %s[%d] unnest 0x%llx->0x%llx: call %lld from user pc %s lr %s; kernel%s",
           name, task_pid(current_task()), start, end, call, pc, lr, path);
}

#endif /* DEVELOPMENT || DEBUG */
