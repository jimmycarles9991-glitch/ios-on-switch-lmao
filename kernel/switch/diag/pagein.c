// SPDX-License-Identifier: GPL-2.0-only
/*
 * Page-ins by process and file (pagein.h), debug kernels: vnode_pager_data_request reports each file pager read after
 * it, on the faulting thread, with the cluster's size and the thread's time in it; vm_page_sleep reports each wait for
 * a busy page, naming the file when the page is one a file read is filling. The dyld shared cache comes in here too
 * (iOS 17's from the cryptex image, iOS 27's translated copy from Data). The 10 s scheduling report adds the
 * "SwitchSched page-ins" and "SwitchSched page-ins by file" lines, starts a new window, and adds NativeSwitchBlock's
 * "SwitchSched cryptex cache" line.
 */

#include <kern/misc_protos.h>
#include <switch/diag.h>
#include <switch/xnu.h>

#include "pagein.h"

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/task.h>
#include <mach/memory_object_types.h>
#include <os/atomic_private.h>
#include <os/log.h>
#include <string.h>

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO

#include <kern/counter.h>
#include <kern/host_statistics.h>
#if XNU_12377_121_6
#include <vm/vm_pageout_xnu.h>
#include <vm/vm_phantom_cache_internal.h>
#else
#include <vm/vm_pageout.h>
#include <vm/vm_phantom_cache.h>
#endif

extern uint32_t vm_phantom_cache_num_entries;
extern const char *vnode_getname(struct vnode *vp);
extern void vnode_putname(const char *name);
extern struct vnode *vnode_pager_lookup_vnode(memory_object_t pager);

static LCK_GRP_DECLARE(pagein_grp, "switch_pagein");
static LCK_SPIN_DECLARE(pagein_lock, &pagein_grp);
static struct switch_pagein_window windows[2];
static struct switch_pagein_window *active_window = &windows[0];
static switch_cryptex_cache_counts_t cryptex_counts;

void switch_diag_pagein(struct vnode *vp, uint64_t bytes, uint64_t read)
{
    const task_t task = current_task();
    char process[SWITCH_PAGEIN_NAME];
    const char *file = NULL;
    bool noted = false;

    switch_task_name(task, process, sizeof(process));
    lck_spin_lock(&pagein_lock);
    noted = switch_pagein_note_read(active_window, task_pid(task), process, (uint64_t)(uintptr_t)vp, NULL, bytes, read);
    lck_spin_unlock(&pagein_lock);
    if (noted) {
        return;
    }
    file = vnode_getname(vp);
    lck_spin_lock(&pagein_lock);
    switch_pagein_note_read(active_window, task_pid(task), process, (uint64_t)(uintptr_t)vp,
                            file != NULL ? file : "(no name)", bytes, read);
    lck_spin_unlock(&pagein_lock);
    if (file != NULL) {
        vnode_putname(file);
    }
}

struct switch_page_wait switch_diag_page_wait(void *pager, unsigned int absent)
{
    struct switch_page_wait wait = {NULL, mach_absolute_time()};

    if (absent && pager != NULL) {
        wait.file = vnode_pager_lookup_vnode((memory_object_t)pager);
    }
    return wait;
}

void switch_diag_page_waited(const struct switch_page_wait *wait)
{
    const uint64_t waited = mach_absolute_time() - wait->began;
    const task_t task = current_task();
    char process[SWITCH_PAGEIN_NAME];

    switch_task_name(task, process, sizeof(process));
    lck_spin_lock(&pagein_lock);
    switch_pagein_note_wait(active_window, task_pid(task), process, (uint64_t)(uintptr_t)wait->file, waited);
    lck_spin_unlock(&pagein_lock);
}

/* XNU's counts since the report before (one caller: the report's thread call). */
static void xnu_counts(struct switch_pagein_xnu *out)
{
    static struct switch_pagein_xnu last;
    const struct switch_pagein_xnu now = {
        .pageins = counter_load(&vm_statistics_pageins),
        .found = vm_pageout_vminfo.vm_phantom_cache_found_ghost,
        .added = vm_pageout_vminfo.vm_phantom_cache_added_ghost,
        .pages = vm_phantom_cache_num_entries * VM_GHOST_PAGES_PER_ENTRY,
    };

    out->pageins = now.pageins - last.pageins;
    out->found = now.found - last.found;
    out->added = now.added - last.added;
    out->pages = now.pages;
    last = now;
}

void switch_diag_pagein_report(char *line, size_t size, uint64_t window_ns)
{
    struct switch_pagein_window *taken;
    struct switch_pagein_xnu xnu;
    const switch_cryptex_cache_counts_t counts = os_atomic_load(&cryptex_counts, acquire);
    mach_timebase_info_data_t tb;
    size_t at = 0;

    clock_timebase_info(&tb);
    lck_spin_lock(&pagein_lock);
    taken = active_window;
    active_window = taken == &windows[0] ? &windows[1] : &windows[0];
    lck_spin_unlock(&pagein_lock);
    at = (size_t)snprintf(line, size, "SwitchSched page-ins (%llu ms): ", window_ns / NSEC_PER_MSEC);
    at = MIN(at, size - 1);
    xnu_counts(&xnu);
    switch_pagein_format(line + at, size - at, taken, &xnu, tb.numer, tb.denom);
    os_log(OS_LOG_DEFAULT, "%s", line);
    at = (size_t)snprintf(line, size,
                          "SwitchSched page-ins by file (%llu ms; reads/KiB asked/ms reading/waits/ms waiting):",
                          window_ns / NSEC_PER_MSEC);
    at = MIN(at, size - 1);
    switch_pagein_format_files(line + at, size - at, taken, tb.numer, tb.denom);
    os_log(OS_LOG_DEFAULT, "%s", line);
    memset(taken, 0, sizeof(*taken));
    if (counts == NULL) {
        return;
    }
    at = (size_t)snprintf(line, size, "SwitchSched cryptex cache (%llu ms): ", window_ns / NSEC_PER_MSEC);
    at = MIN(at, size - 1);
    if (counts(line + at, size - at) != 0) {
        os_log(OS_LOG_DEFAULT, "%s", line);
    }
}

/* The release store publishes the driver's function before the report's acquire load calls it. */
void switch_diag_cryptex_cache(switch_cryptex_cache_counts_t counts)
{
    os_atomic_store(&cryptex_counts, counts, release);
}

#elif DEVELOPMENT || DEBUG

void switch_diag_pagein_report(__unused char *line, __unused size_t size, __unused uint64_t window_ns)
{}

void switch_diag_cryptex_cache(__unused switch_cryptex_cache_counts_t counts)
{}

#endif /* DEVELOPMENT || DEBUG */
