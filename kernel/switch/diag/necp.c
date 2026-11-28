// SPDX-License-Identifier: GPL-2.0-only
/*
 * A local-network drop NECP reports for a process (<switch/necp.h>), debug kernels: two lines for the first drop of
 * each of SWITCH_NECP_PROBE_PIDS processes, and the policy table around any drop that takes a dump (diag/necp_table.c).
 * task-platform is the task's exec-time flag; cs-platform is csproc_get_platform_binary, the value NECP ands with
 * cs_valid.
 */
#include <switch/bootstrap.h>
#include <switch/necp.h>

#include <kern/task.h>
#include <os/log.h>
#include <sys/codesign.h>
#include <sys/proc_internal.h>
#include <sys/vm.h>

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_DIAG_NECP) && SWITCH_DIAG_NECP

static int32_t switch_necp_pids[SWITCH_NECP_PROBE_PIDS];

static int switch_necp_task_platform(proc_t p)
{
    const task_t task = p != PROC_NULL ? proc_task(p) : TASK_NULL;
    return task != TASK_NULL ? (int)task_get_platform_binary(task) : -1;
}

static int switch_necp_cs_platform(proc_t p)
{
    return p != PROC_NULL ? csproc_get_platform_binary(p) : -1;
}

/* The process necp_application_fillout_info_locked or necp_socket_fillout_info_locked judged platform conditions on:
 * the delegating one for a delegated flow, the responsible one when NECP used it, else the process itself. */
static proc_t switch_necp_judged(const struct switch_necp_drop *drop, int32_t *pid)
{
    if (drop->delegated) {
        if (drop->caller != PROC_NULL) {
            *pid = proc_getpid(drop->caller);
            return drop->caller;
        }
        *pid = drop->last_pid;
        if (drop->last_pid == proc_selfpid()) {
            return current_proc();
        }
        return drop->last_pid == drop->pid ? drop->process : PROC_NULL;
    }
    if (drop->used_responsible) {
        *pid = drop->responsible != PROC_NULL ? proc_getpid(drop->responsible) : drop->responsible_pid;
        return drop->responsible;
    }
    *pid = drop->pid;
    return drop->process;
}

static void switch_necp_log_drop(const struct switch_necp_drop *drop)
{
    const proc_t process = drop->process;
    int32_t judged_pid = 0;
    const proc_t judged = switch_necp_judged(drop, &judged_pid);
    os_log(OS_LOG_DEFAULT,
           "SwitchNECP: probe local-network drop site %u pid %d (%s) uid %u task-platform %d cs-platform %d "
           "csflags 0x%llx injected %d delegated %d judged pid %d (%s) task-platform %d cs-platform %d csflags 0x%llx",
           drop->site, drop->pid, process != PROC_NULL ? proc_best_name(process) : "?", drop->uid,
           switch_necp_task_platform(process), switch_necp_cs_platform(process),
           process != PROC_NULL ? proc_getcsflags(process) : 0ull,
           process != PROC_NULL && switch_bootstrap_process(process), drop->delegated, judged_pid,
           judged != PROC_NULL ? proc_best_name(judged) : "?", switch_necp_task_platform(judged),
           switch_necp_cs_platform(judged), judged != PROC_NULL ? proc_getcsflags(judged) : 0ull);
    os_log(OS_LOG_DEFAULT,
           "SwitchNECP: probe local-network drop pid %d policy id %u session-order %u order %u conditions 0x%llx "
           "negated 0x%llx result-flags 0x%x necp-platform %d real-platform %d skip-installed %d skip id %u",
           drop->pid, drop->policy_id, drop->session_order, drop->order, drop->conditions, drop->negated,
           drop->drop_flags, drop->necp_platform, drop->necp_real_platform, drop->skip_installed, drop->skip_id);
}

void switch_necp_local_drop(const struct switch_necp_drop *drop)
{
    if (switch_necp_probe_claim(switch_necp_pids, SWITCH_NECP_PROBE_PIDS, drop->pid)) {
        switch_necp_log_drop(drop);
    }
    switch_necp_table_dump(drop);
}

#endif /* (DEVELOPMENT || DEBUG) && SWITCH_DIAG_NECP */
