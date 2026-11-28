// SPDX-License-Identifier: GPL-2.0-only
/*
 * A shared-region unnest XNU logs (log_unnest_badness, debug kernels): who asked for it. XNU's line names the process
 * and the range; this one adds the user code that trapped in (its PC, LR and frame chain, as dyld shared cache or
 * main-image offsets), the call it made (x16: a BSD system call above 0, a Mach trap below), and the kernel path from
 * the unnest up, unslid, for atos. It rides XNU's own rate limit.
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
#include <string.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_shared_region.h>
#if XNU_12377_121_6
#include <vm/vm_map_xnu.h>
#include <vm/vm_shared_region_xnu.h>
#endif

#if DEVELOPMENT || DEBUG

#define UNNEST_FRAMES             8
#define UNNEST_UFRAMES            8
#define UNNEST_SWIFT_ASYNC_FP_BIT (1ull << 60)

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

static uint64_t unnest_strip_fp(uint64_t fp)
{
    fp &= ~UNNEST_SWIFT_ASYNC_FP_BIT;
    return (uint64_t)(uintptr_t)ptrauth_strip((void *)(uintptr_t)fp, ptrauth_key_frame_pointer);
}

/*
 * log_unnest_badness() runs with the task's vm_map lock held, so a faulting read would deadlock on recursive
 * vm_map_lock. pmap_find_pa_nofault() translates the current user VA with AT S1E0R (zero locks) and returns an exact
 * byte physical address at the 4 KiB hardware granule, avoiding pmap_find_phys_nofault()'s lossy atop() shift when
 * SWITCH_VM_16K_PAGES=1.
 */
static bool unnest_read_u64(pmap_t pmap, uint64_t va, uint64_t *out)
{
    pmap_paddr_t pa = 0;
    uint64_t value = 0;

    if (va == 0 || (va & 7u) != 0 || va > MACH_VM_MAX_ADDRESS - sizeof(*out)) {
        return false;
    }
    pa = pmap_find_pa_nofault(pmap, (addr64_t)va);
    if (pa == 0 || !pmap_valid_address(pa)) {
        return false;
    }
    memcpy(&value, (const void *)(uintptr_t)phystokv(pa), sizeof(value));
    *out = value;
    return true;
}

static void unnest_walk_user(const arm_saved_state_t *user, char *out, size_t size)
{
    const vm_map_t map = current_thread()->map;
    pmap_t pmap = PMAP_NULL;
    uint64_t fp = 0;
    size_t at = 0;

    if (size == 0) {
        return;
    }
    out[0] = '\0';
    if (!is_saved_state64(user) || map == VM_MAP_NULL || map != current_task()->map) {
        return;
    }
    pmap = vm_map_pmap(map);
    if (pmap == PMAP_NULL || pmap == kernel_pmap) {
        return;
    }
    fp = unnest_strip_fp(get_saved_state_fp(user));
    for (unsigned int i = 0; i < UNNEST_UFRAMES; i++) {
        uint64_t next_fp = 0;
        uint64_t ret = 0;
        char where[32] = "";

        if (!unnest_read_u64(pmap, fp, &next_fp) || !unnest_read_u64(pmap, fp + sizeof(uint64_t), &ret)) {
            break;
        }
        ret = (uint64_t)(uintptr_t)ptrauth_strip((void *)(uintptr_t)ret, ptrauth_key_return_address);
        if (ret == 0) {
            break;
        }
        unnest_where(where, sizeof(where), ret);
        at = switch_busy_put(size, at, snprintf(out + at, size - at, "%s %s", i == 0 ? ", frames" : "", where));
        next_fp = unnest_strip_fp(next_fp);
        if (next_fp <= fp) {
            break;
        }
        fp = next_fp;
    }
}

void switch_diag_unnest(uint64_t start, uint64_t end)
{
    const arm_saved_state_t *user = get_user_regs(current_thread());
    uintptr_t frames[UNNEST_FRAMES] = {0};
    const unsigned int n = backtrace(frames, UNNEST_FRAMES, NULL, NULL);
    char name[32], pc[32] = "-", lr[32] = "-", uframes[UNNEST_UFRAMES * 24 + 16] = "", path[UNNEST_FRAMES * 20] = "";
    long long call = 0;
    size_t at = 0;

    if (user != NULL) {
        unnest_where(pc, sizeof(pc), get_saved_state_pc(user));
        unnest_where(lr, sizeof(lr), get_saved_state_lr(user));
        call = (long long)get_saved_state_reg(user, 16);
        unnest_walk_user(user, uframes, sizeof(uframes));
    }
    /* frames[0] is in this function */
    for (unsigned int i = 1; i < n; i++) {
        at = switch_busy_put(
            sizeof(path), at,
            snprintf(path + at, sizeof(path) - at, " 0x%lx", (unsigned long)VM_KERNEL_UNSLIDE(frames[i])));
    }
    switch_task_name(current_task(), name, sizeof(name));
    os_log(OS_LOG_DEFAULT, "SwitchUnnest: %s[%d] unnest 0x%llx->0x%llx: call %lld from user pc %s lr %s%s; kernel%s",
           name, task_pid(current_task()), start, end, call, pc, lr, uframes, path);
}

#endif /* DEVELOPMENT || DEBUG */
