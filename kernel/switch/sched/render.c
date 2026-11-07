// SPDX-License-Identifier: GPL-2.0-only
/*
 * The render thread's frame (render.h), debug kernels: one "SwitchFrameRender:" line for each swap that came half a refresh
 * period or more after the vsync before it, saying where Core Animation's display thread spent that time: on a core
 * (QuartzCore's own work and our Metal driver's, for kprof to split), blocked (and where), or waiting for a core. The
 * display reports each vsync and swap and the GPU driver each compositor submission, all on the thread itself except the
 * vsync; the thread is sched/stats.c's display.primary, whose dispatches it sees under the thread lock. tools/frames
 * joins the lines to the display's "SwitchFrame:" ones by the vsync stamp.
 */

#include <switch/sched.h>

#include "../diag/stall.h"
#include "render.h"

#include <arm/machine_routines.h>
#include <arm/thread.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <kern/recount.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <ptrauth.h>
#include <string.h>
#include <vm/vm_shared_region.h>
#if XNU_12377_121_6
#include <vm/vm_shared_region_xnu.h>
#endif

#if DEVELOPMENT || DEBUG

#define LINES_PER_SECOND 30
#define MIN_BLOCK_US     10

/* The thread's own dispatches and the thread itself are the only writers, and never at once. */
static struct switch_render_window window;
static uint64_t longest_pc, longest_lr, longest_kernel;

static uint64_t render_thread, vsync_stamp, vsync_count, half_period;
static uint64_t lines_since;
static uint32_t lines, dropped;

void switch_sched_render_vsync(uint64_t stamp, uint64_t period)
{
    os_atomic_store(&vsync_stamp, stamp, relaxed);
    os_atomic_store(&half_period, period / 2, relaxed);
    os_atomic_inc(&vsync_count, release);
}

void switch_sched_render_on_core(thread_t thread, uint64_t now)
{
    static uint64_t min_block;
    const uint64_t made = thread->last_made_runnable_time;
    const uint64_t count = os_atomic_load(&vsync_count, acquire);

    if (min_block == 0) {
        nanoseconds_to_absolutetime(MIN_BLOCK_US * NSEC_PER_USEC, &min_block);
    }
    os_atomic_store(&render_thread, thread->thread_id, relaxed);
    if (switch_render_dispatched(&window, count, os_atomic_load(&vsync_stamp, relaxed), now,
                                 recount_thread_time_mach(thread), made == THREAD_NOT_RUNNABLE ? 0 : made, min_block)) {
        const arm_saved_state_t *user = get_user_regs(thread);

        longest_pc = user != NULL ? get_saved_state_pc(user) : 0;
        longest_lr = user != NULL ? get_saved_state_lr(user) : 0;
        longest_kernel = thread->continuation != NULL ? VM_KERNEL_UNSLIDE((uintptr_t)ptrauth_strip(
                                                            (void *)thread->continuation, ptrauth_key_function_pointer))
                                                      : 0;
    }
}

static bool on_render_thread(void)
{
    return current_thread()->thread_id == os_atomic_load(&render_thread, relaxed);
}

static uint64_t own_cpu(void)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const uint64_t cpu = recount_current_thread_time_mach();

    ml_set_interrupts_enabled(enabled);
    return cpu;
}

void switch_sched_render_submitted(void)
{
    if (on_render_thread()) {
        switch_render_submitted(&window, mach_absolute_time(), own_cpu());
    }
}

static uint64_t us(uint64_t ticks)
{
    uint64_t ns;

    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / NSEC_PER_USEC;
}

static long long us_after(uint64_t from, uint64_t to)
{
    return to >= from ? (long long)us(to - from) : -(long long)us(from - to);
}

static void user_address(char *out, size_t size, uint64_t pc)
{
    const task_t task = current_task();
    const struct vm_shared_region *sr = task->shared_region;
    uint64_t value;

    pc = (uint64_t)ptrauth_strip((void *)pc, ptrauth_key_return_address);
    switch (switch_stall_user_where(pc, sr ? sr->sr_base_address : 0, sr ? sr->sr_size : 0, sr ? sr->sr_slide : 0,
                                    task->mach_header_vm_address, &value)) {
    case SWITCH_STALL_DSC:
        snprintf(out, size, "dsc 0x%llx", value);
        break;
    case SWITCH_STALL_MAIN:
        snprintf(out, size, "main+0x%llx", value);
        break;
    default:
        snprintf(out, size, "0x%llx", value);
        break;
    }
}

static bool line_allowed(uint64_t now, uint32_t *skipped)
{
    uint64_t second;

    nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
    if (now - lines_since > second) {
        lines_since = now;
        lines = 0;
    }
    if (lines++ >= LINES_PER_SECOND) {
        dropped++;
        return false;
    }
    *skipped = dropped;
    dropped = 0;
    return true;
}

void switch_sched_render_swapped(void)
{
    const struct switch_render_window *w = &window;
    char first[96], pc[32], lr[32], line[640];
    uint64_t now, cpu;
    uint32_t skipped = 0;

    if (!on_render_thread() || w->woke == 0 || w->swapped) {
        return;
    }
    window.swapped = true;
    now = mach_absolute_time();
    if (now - w->vsync < os_atomic_load(&half_period, relaxed) || !line_allowed(now, &skipped)) {
        return;
    }
    cpu = own_cpu() - w->cpu0;
    if (w->first != 0) {
        snprintf(first, sizeof(first), "%lld us (cpu %llu, blocked %llu us in %u)", us_after(w->vsync, w->first),
                 us(w->first_cpu), us(w->first_blocked), w->first_blocks);
    } else {
        strlcpy(first, "none", sizeof(first));
    }
    if (w->longest != 0) {
        user_address(pc, sizeof(pc), longest_pc);
        user_address(lr, sizeof(lr), longest_lr);
    } else {
        strlcpy(pc, "-", sizeof(pc));
        strlcpy(lr, "-", sizeof(lr));
    }
    snprintf(line, sizeof(line),
             "vsync %llu: woke %lld us after it; first submit %s; swap %lld us (cpu %llu, blocked %llu us in %u, "
             "waiting for a core %llu us); longest block %llu us, user pc %s lr %s, kernel continuation 0x%llx; "
             "dropped %u",
             w->vsync, us_after(w->vsync, w->woke), first, us_after(w->vsync, now), us(cpu), us(w->blocked), w->blocks,
             us(w->runnable), us(w->longest), pc, lr, w->longest ? longest_kernel : 0, skipped);
    os_log(OS_LOG_DEFAULT, "SwitchFrameRender: %s", line);
}

#endif /* DEVELOPMENT || DEBUG */
