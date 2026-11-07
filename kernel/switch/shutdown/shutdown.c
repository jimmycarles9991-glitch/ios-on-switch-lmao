// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shut down and restart (<switch/shutdown.h>). Debug kernels: reboot(2)'s start and every step reboot_kernel and the
 * IOKit halt path finish go to the power trail, as lines and as durable marks (SwitchPowerTrail.h), each mark in the
 * RTC before the next step starts; then the SoC watchdog is armed, and a shutdown still running DEADLINE_S after
 * reboot(2) flags that on the mark of the step it is stuck after and panics, naming that step. The durable mark
 * survives POWER held; the panic's record and the trail survive only a warm reset that gets back to iPadOS. XNU's own
 * halt timeout ("Halt/Restart Timed Out") starts only at PEHaltRestart. A healthy shutdown is over in a few seconds.
 *
 * debug.switch_watchdog_bench (debug kernels, written by BlueTool when the Mac posts
 * com.brunostjohn.switch.debug.watchdog-bench): the SoC watchdog armed for that many seconds and never petted, with a
 * durable mark first. A console back in hekate shows a watchdog and a warm reset that work; one left dark, a reset
 * that does not come back; one still running, a watchdog that never fires.
 */
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/thread_call.h>
#include <mach/vm_param.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/reboot.h>
#include <sys/sysctl.h>
#include <switch/shutdown.h>
#include "SwitchPowerTrail.h"

#if DEVELOPMENT || DEBUG
#define DEADLINE_S 25u
#define WATCHDOG_S 45u

static int shutdown_howto;
static const char *volatile last_step = "none";
static unsigned procs_done, unmounts_done;

/* A mark in the RTC before the caller's next step. */
static void switch_shutdown_mark_sync(unsigned code)
{
    switch_power_trail_mark(code);
    switch_power_trail_mark_flush();
}

static void switch_shutdown_stalled(thread_call_param_t p0 __unused, thread_call_param_t p1 __unused)
{
    switch_power_trail_flag_now(SWITCH_MARK_FLAG_DEADLINE);
    panic("SwitchShutdown: reboot(%#x) still running %u s after it began; last step done: %s", shutdown_howto,
          DEADLINE_S, last_step);
}

/* The durable mark for a halt_log_enter step (reboot_kernel and IOPMrootDomain.cpp name them). */
static unsigned switch_shutdown_mark(const char *what)
{
    static const struct {
        const char *what;
        unsigned mark;
    } steps[] = {
        {"IOPMRootDomainWillShutdown", SWITCH_MARK_HALT_NOTIFIED},
        {"PagingOff", SWITCH_MARK_HALT_NOTIFIED},
        {"bufferclean", SWITCH_MARK_HALT_BUFFERS},
        {"if_down_all", SWITCH_MARK_HALT_INTERFACES},
        {"PowerOff/Restart still waiting on handler", SWITCH_MARK_HALT_DRIVER_SLOW},
        {"Quiesce", SWITCH_MARK_HALT_QUIESCED},
        {"PowerOff", SWITCH_MARK_HALT_DRIVERS},
        {"Restart", SWITCH_MARK_HALT_DRIVERS},
    };

    if (!strcmp(what, "proc_shutdown"))
        return procs_done++ ? SWITCH_MARK_HALT_PROCS_ALL : SWITCH_MARK_HALT_PROCS;
    if (!strcmp(what, "vfs_unmountall"))
        return unmounts_done++ ? SWITCH_MARK_HALT_UNMOUNTED_ALL : SWITCH_MARK_HALT_UNMOUNTED;
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        if (!strcmp(what, steps[i].what))
            return steps[i].mark;
    }
    return SWITCH_MARK_HALT_OTHER;
}

void switch_shutdown_begin(int howto)
{
    shutdown_howto = howto;
    switch_power_trail("reboot(%#x) began", howto);
    switch_shutdown_mark_sync(SWITCH_MARK_HALT_BEGAN);
    switch_power_trail_watch(WATCHDOG_S, "shutdown");
    thread_call_t call = thread_call_allocate_with_options(switch_shutdown_stalled, NULL,
                                                           THREAD_CALL_PRIORITY_KERNEL_HIGH, THREAD_CALL_OPTIONS_ONCE);
    if (!call) {
        switch_power_trail("no shutdown deadline: no thread call");
        return;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(DEADLINE_S, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(call, deadline);
}

void switch_shutdown_step(const char *what, const void *pc, uint64_t elapsed)
{
    if (!get_system_inshutdown())
        return;
    last_step = what;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(elapsed, &ns);
    if (pc)
        switch_power_trail("%s %llu ms at %#lx", what, ns / NSEC_PER_MSEC, (unsigned long)VM_KERNEL_UNSLIDE(pc));
    else
        switch_power_trail("%s %llu ms", what, ns / NSEC_PER_MSEC);
    switch_shutdown_mark_sync(switch_shutdown_mark(what));
}

static int switch_watchdog_bench SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
    int seconds = 0;
    int changed = 0;
    const int error = sysctl_io_number(req, 0, sizeof(seconds), &seconds, &changed);
    if (error || !changed)
        return error;
    if (seconds < 5 || seconds > 255)
        return EINVAL;
    switch_power_trail("watchdog bench: %d s, never petted", seconds);
    switch_power_trail_mark(SWITCH_MARK_WATCHDOG_BENCH);
    switch_power_trail_mark_flush();
    switch_power_trail_watch((unsigned)seconds, "bench");
    return 0;
}

SYSCTL_PROC(_debug, OID_AUTO, switch_watchdog_bench, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL,
            0, switch_watchdog_bench, "I", "SoC watchdog bench: seconds to a reset, never petted");
#else
void switch_shutdown_begin(int howto __unused)
{}

void switch_shutdown_step(const char *what __unused, const void *pc __unused, uint64_t elapsed __unused)
{}
#endif
