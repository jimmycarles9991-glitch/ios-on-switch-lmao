// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bootstrap/threads/memory.h"
#include "bootstrap/threads/procs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);

enum {
    SWITCH_PROC_PIDREGIONINFO = 7,
};

struct switch_proc_regioninfo {
    uint32_t pri_protection;
    uint32_t pri_max_protection;
    uint32_t pri_inheritance;
    uint32_t pri_flags;
    uint64_t pri_offset;
    uint32_t pri_behavior;
    uint32_t pri_user_wired_count;
    uint32_t pri_user_tag;
    uint32_t pri_pages_resident;
    uint32_t pri_pages_shared_now_private;
    uint32_t pri_pages_swapped_out;
    uint32_t pri_pages_dirtied;
    uint32_t pri_ref_count;
    uint32_t pri_shadow_depth;
    uint32_t pri_share_mode;
    uint32_t pri_private_pages_resident;
    uint32_t pri_shared_pages_resident;
    uint32_t pri_obj_id;
    uint32_t pri_depth;
    uint64_t pri_address;
    uint64_t pri_size;
};

_Static_assert(sizeof(struct switch_proc_regioninfo) == 96, "proc_regioninfo ABI mismatch");

struct region_list {
    struct switch_vm_region *items;
    size_t count;
    size_t capacity;
};

static bool region_list_push(struct region_list *list, const struct switch_vm_region *item)
{
    if (list->count == list->capacity) {
        const size_t next = list->capacity ? list->capacity * 2 : 1024;
        struct switch_vm_region *grown = realloc(list->items, next * sizeof(*grown));
        if (!grown)
            return false;
        list->items = grown;
        list->capacity = next;
    }
    list->items[list->count++] = *item;
    return true;
}

// With vm.self_region_footprint set, each region's dirtied and swapped-out pages are the ones charged to the target's
// phys_footprint, and the walk ends with the owned-memory region. The kernel counts pages in the size pinned by
// vm.self_region_page_size. The footprint switch is this task's; the page size is this thread's.
bool switch_memory_footprint_mode(uint64_t *page_size)
{
    int on = 1;
    int size = getpagesize();
    if (sysctlbyname("vm.self_region_footprint", NULL, NULL, &on, sizeof(on)) != 0) {
        perror("switch-threads: vm.self_region_footprint");
        return false;
    }
    if (sysctlbyname("vm.self_region_page_size", NULL, NULL, &size, sizeof(size)) != 0) {
        perror("switch-threads: vm.self_region_page_size");
        return false;
    }
    *page_size = (uint64_t)size;
    return true;
}

// The kernel answers EINVAL once no region starts at or after the address: the walk's normal end.
static bool walk_regions(int pid, struct region_list *list)
{
    uint64_t address = 0;
    for (;;) {
        struct switch_proc_regioninfo info;
        memset(&info, 0, sizeof(info));
        errno = 0;
        if (proc_pidinfo(pid, SWITCH_PROC_PIDREGIONINFO, address, &info, (int)sizeof(info)) != (int)sizeof(info))
            return errno == EINVAL;
        const struct switch_vm_region region = {
            .tag = info.pri_user_tag,
            .pages_resident = info.pri_pages_resident,
            .pages_dirtied = info.pri_pages_dirtied,
            .pages_swapped_out = info.pri_pages_swapped_out,
            .size = info.pri_size,
        };
        if (!region_list_push(list, &region))
            return false;
        const uint64_t next = info.pri_address + info.pri_size;
        if (next <= address)
            return true;
        address = next;
    }
}

bool switch_memory_walk(int pid, uint64_t page_size, struct switch_vm_tags *tags, struct switch_vm_snapshot *snap)
{
    struct rusage_info_v4 usage;
    memset(&usage, 0, sizeof(usage));
    if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&usage) != 0) {
        fprintf(stderr, "switch-threads: proc_pid_rusage(%d): %s\n", pid, strerror(errno));
        return false;
    }
    struct region_list regions = {NULL, 0, 0};
    if (!walk_regions(pid, &regions)) {
        fprintf(stderr, "switch-threads: region walk of %d: %s\n", pid, strerror(errno));
        free(regions.items);
        return false;
    }
    tags->kept = switch_vm_tags_sum(regions.items, regions.count, page_size, tags->rows, &tags->total);
    free(regions.items);
    snap->pid = pid;
    snap->uptime_s = (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    snap->footprint_bytes = usage.ri_phys_footprint;
    snap->peak_bytes = usage.ri_lifetime_max_phys_footprint;
    return true;
}

static bool report_pid(int pid, const char *name, uint64_t page_size, size_t limit)
{
    struct switch_vm_tags tags;
    struct switch_vm_snapshot snap = {.name = name};
    if (!switch_memory_walk(pid, page_size, &tags, &snap))
        return false;
    switch_vm_tags_format(stdout, &snap, tags.rows, tags.kept, &tags.total, limit);
    fflush(stdout);
    return true;
}

static void sleep_interval(unsigned long ms)
{
    struct timespec req = {.tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)((ms % 1000) * 1000000UL)};
    struct timespec rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
}

int switch_memory_report(const char *name, int pid, unsigned count, unsigned long interval_ms, size_t limit)
{
    uint64_t page_size = 0;
    if (!switch_memory_footprint_mode(&page_size))
        return 1;
    bool any = false;
    for (unsigned i = 0; i < count; ++i) {
        if (i)
            sleep_interval(interval_ms);
        if (!name) {
            any |= report_pid(pid, "-", page_size, limit);
            continue;
        }
        int pids[16];
        const size_t found = switch_pids_named(name, pids, sizeof(pids) / sizeof(pids[0]));
        if (!found)
            printf("t=%.1fs %s: not running\n", (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9, name);
        for (size_t p = 0; p < found; ++p)
            any |= report_pid(pids[p], name, page_size, limit);
        fflush(stdout);
    }
    return any ? 0 : 1;
}
