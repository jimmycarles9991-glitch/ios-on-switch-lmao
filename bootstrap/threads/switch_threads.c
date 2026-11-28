// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bootstrap/threads/memory.h"
#include "bootstrap/threads/procs.h"
#include "bootstrap/threads/threads.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

enum {
    SWITCH_PROC_PIDTHREADINFO = 5,
    SWITCH_PROC_PIDLISTTHREADS = 6,
    SWITCH_PROC_PIDT_SHORTBSDINFO = 13,
    SWITCH_PROC_PIDTHREADID64INFO = 15,
    SWITCH_PROC_PIDLISTTHREADIDS = 28,
};

struct switch_proc_bsdshortinfo {
    uint32_t pbsi_pid;
    uint32_t pbsi_ppid;
    uint32_t pbsi_pgid;
    uint32_t pbsi_status;
    char pbsi_comm[16];
    uint32_t pbsi_flags;
    uid_t pbsi_uid;
    gid_t pbsi_gid;
    uid_t pbsi_ruid;
    gid_t pbsi_rgid;
    uid_t pbsi_svuid;
    gid_t pbsi_svgid;
    uint32_t pbsi_rfu;
};

struct switch_proc_threadinfo {
    uint64_t pth_user_time;
    uint64_t pth_system_time;
    int32_t pth_cpu_usage;
    int32_t pth_policy;
    int32_t pth_run_state;
    int32_t pth_flags;
    int32_t pth_sleep_time;
    int32_t pth_curpri;
    int32_t pth_priority;
    int32_t pth_maxpriority;
    char pth_name[64];
};

_Static_assert(sizeof(struct switch_proc_bsdshortinfo) == 64, "proc_bsdshortinfo ABI mismatch");
_Static_assert(sizeof(struct switch_proc_threadinfo) == 112, "proc_threadinfo ABI mismatch");

struct sample_list {
    struct switch_thread_sample *items;
    size_t count;
    size_t capacity;
};

static bool sample_list_push(struct sample_list *list, const struct switch_thread_sample *item)
{
    if (list->count == list->capacity) {
        const size_t next = list->capacity ? list->capacity * 2 : 256;
        struct switch_thread_sample *grown = realloc(list->items, next * sizeof(*grown));
        if (!grown)
            return false;
        list->items = grown;
        list->capacity = next;
    }
    list->items[list->count++] = *item;
    return true;
}

static bool sample_pid(pid_t pid, struct sample_list *list)
{
    struct switch_proc_bsdshortinfo bsd;
    memset(&bsd, 0, sizeof(bsd));
    if (proc_pidinfo(pid, SWITCH_PROC_PIDT_SHORTBSDINFO, 0, &bsd, (int)sizeof(bsd)) != (int)sizeof(bsd))
        return true;

    int list_flavor = SWITCH_PROC_PIDLISTTHREADIDS;
    int info_flavor = SWITCH_PROC_PIDTHREADID64INFO;
    size_t capacity = 64;
    uint64_t *tids = NULL;
    int bytes = 0;

    for (;;) {
        uint64_t *grown = realloc(tids, capacity * sizeof(*tids));
        if (!grown) {
            free(tids);
            return false;
        }
        tids = grown;
        bytes = proc_pidinfo(pid, list_flavor, 0, tids, (int)(capacity * sizeof(*tids)));
        if (bytes <= 0 && list_flavor == SWITCH_PROC_PIDLISTTHREADIDS) {
            list_flavor = SWITCH_PROC_PIDLISTTHREADS;
            info_flavor = SWITCH_PROC_PIDTHREADINFO;
            bytes = proc_pidinfo(pid, list_flavor, 0, tids, (int)(capacity * sizeof(*tids)));
        }
        if (bytes <= 0) {
            free(tids);
            return true;
        }
        if ((size_t)bytes < capacity * sizeof(*tids) || capacity >= 16384)
            break;
        capacity *= 2;
    }

    const size_t count = (size_t)bytes / sizeof(*tids);
    for (size_t i = 0; i < count; ++i) {
        struct switch_proc_threadinfo info;
        memset(&info, 0, sizeof(info));
        if (proc_pidinfo(pid, info_flavor, tids[i], &info, (int)sizeof(info)) != (int)sizeof(info))
            continue;
        struct switch_thread_sample sample;
        memset(&sample, 0, sizeof(sample));
        sample.pid = pid;
        sample.tid = tids[i];
        sample.user_ns = info.pth_user_time;
        sample.sys_ns = info.pth_system_time;
        sample.cpu_permille = info.pth_cpu_usage;
        sample.run_state = info.pth_run_state;
        sample.pri = info.pth_curpri;
        memcpy(sample.proc, bsd.pbsi_comm, sizeof(bsd.pbsi_comm));
        memcpy(sample.name, info.pth_name, sizeof(info.pth_name));
        if (!sample_list_push(list, &sample)) {
            free(tids);
            return false;
        }
    }
    free(tids);
    return true;
}

static bool collect_samples(int target_pid, const char *name, struct sample_list *list)
{
    if (target_pid >= 0)
        return sample_pid(target_pid, list);
    int count = 0;
    int *pids = switch_all_pids(&count);
    if (!pids)
        return false;
    bool ok = true;
    for (int i = 0; i < count && ok; ++i) {
        if (pids[i] > 0 && (!name || switch_pid_is_named(pids[i], name)))
            ok = sample_pid(pids[i], list);
    }
    free(pids);
    return ok;
}

static void sleep_ms(unsigned long ms)
{
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000UL);
    struct timespec rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
}

int main(int argc, char **argv)
{
    unsigned long interval_ms = 1000;
    size_t limit = 200;
    int target_pid = -1;
    const char *name = NULL;
    bool memory = false;
    unsigned snapshots = 1;
    int opt;
    while ((opt = getopt(argc, argv, "s:n:p:c:l:m")) != -1) {
        if (opt == 'm') {
            memory = true;
            continue;
        }
        if (opt == 'c') {
            name = optarg;
            continue;
        }
        if (opt == '?')
            return 2;
        char *end = NULL;
        errno = 0;
        const long value = strtol(optarg, &end, 10);
        if (errno || !end || *end != '\0' || value < 0) {
            fprintf(stderr, "switch-threads: invalid -%c value: %s\n", opt, optarg);
            return 2;
        }
        if (opt == 's')
            interval_ms = (unsigned long)value;
        else if (opt == 'n')
            limit = (size_t)value;
        else if (opt == 'p')
            target_pid = (int)value;
        else if (opt == 'l')
            snapshots = (unsigned)value;
    }

    if (memory) {
        if (!name && target_pid < 0) {
            fprintf(stderr, "switch-threads: -m needs -c NAME or -p PID\n");
            return 2;
        }
        return switch_memory_report(name, target_pid, snapshots, interval_ms, limit);
    }

    struct sample_list before = {NULL, 0, 0};
    struct sample_list after = {NULL, 0, 0};
    uint64_t elapsed_ns = 0;

    if (interval_ms > 0) {
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (!collect_samples(target_pid, name, &before)) {
            free(before.items);
            return 1;
        }
        sleep_ms(interval_ms);
        const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        elapsed_ns = t1 > t0 ? t1 - t0 : 1;
    }

    if (!collect_samples(target_pid, name, &after)) {
        free(before.items);
        free(after.items);
        return 1;
    }

    struct switch_thread_row *rows = after.count ? calloc(after.count, sizeof(*rows)) : NULL;
    if (after.count && !rows) {
        free(before.items);
        free(after.items);
        return 1;
    }

    const size_t total = switch_threads_compute(before.items, before.count, after.items, after.count, elapsed_ns, rows);
    switch_threads_format(stdout, rows, total, limit);

    free(rows);
    free(before.items);
    free(after.items);
    return 0;
}
