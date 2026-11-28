// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_THREAD_PROC_MAX 17
#define SWITCH_THREAD_NAME_MAX 65

struct switch_thread_sample {
    int32_t pid;
    uint64_t tid;
    uint64_t user_ns;
    uint64_t sys_ns;
    int32_t cpu_permille;
    int32_t run_state;
    int32_t pri;
    char proc[SWITCH_THREAD_PROC_MAX];
    char name[SWITCH_THREAD_NAME_MAX];
};

struct switch_thread_row {
    int32_t pid;
    uint64_t tid;
    double cpu_pct;
    double user_ms;
    double sys_ms;
    double total_ms;
    char state;
    int32_t pri;
    char proc[SWITCH_THREAD_PROC_MAX];
    char name[SWITCH_THREAD_NAME_MAX];
};

static inline char switch_thread_state_char(int32_t state)
{
    switch (state) {
    case 1:
        return 'R';
    case 2:
        return 'T';
    case 3:
        return 'S';
    case 4:
        return 'U';
    case 5:
        return 'H';
    default:
        return '?';
    }
}

static inline void switch_thread_sanitize(char *dst, size_t cap, const char *src)
{
    if (!cap)
        return;
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; ++i) {
        const unsigned char ch = (unsigned char)src[i];
        dst[i] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : ' ';
    }
    dst[i] = '\0';
}

static inline int switch_thread_sample_cmp(const void *lhs, const void *rhs)
{
    const struct switch_thread_sample *a = (const struct switch_thread_sample *)lhs;
    const struct switch_thread_sample *b = (const struct switch_thread_sample *)rhs;
    if (a->pid != b->pid)
        return a->pid < b->pid ? -1 : 1;
    if (a->tid != b->tid)
        return a->tid < b->tid ? -1 : 1;
    return 0;
}

static inline int switch_thread_row_cmp(const void *lhs, const void *rhs)
{
    const struct switch_thread_row *a = (const struct switch_thread_row *)lhs;
    const struct switch_thread_row *b = (const struct switch_thread_row *)rhs;
    if (a->cpu_pct != b->cpu_pct)
        return a->cpu_pct > b->cpu_pct ? -1 : 1;
    if (a->total_ms != b->total_ms)
        return a->total_ms > b->total_ms ? -1 : 1;
    if (a->pid != b->pid)
        return a->pid < b->pid ? -1 : 1;
    if (a->tid != b->tid)
        return a->tid < b->tid ? -1 : 1;
    return 0;
}

static inline uint64_t switch_thread_delta(uint64_t cur, uint64_t prev)
{
    return cur >= prev ? cur - prev : 0;
}

static inline size_t switch_threads_compute(struct switch_thread_sample *before, size_t before_count,
                                            const struct switch_thread_sample *after, size_t after_count,
                                            uint64_t elapsed_ns, struct switch_thread_row *out)
{
    if (before && before_count > 1)
        qsort(before, before_count, sizeof(*before), switch_thread_sample_cmp);
    for (size_t i = 0; i < after_count; ++i) {
        const struct switch_thread_sample *cur = &after[i];
        struct switch_thread_row *row = &out[i];
        row->pid = cur->pid;
        row->tid = cur->tid;
        row->user_ms = (double)cur->user_ns / 1e6;
        row->sys_ms = (double)cur->sys_ns / 1e6;
        row->total_ms = row->user_ms + row->sys_ms;
        row->state = switch_thread_state_char(cur->run_state);
        row->pri = cur->pri;
        switch_thread_sanitize(row->proc, sizeof(row->proc), cur->proc);
        switch_thread_sanitize(row->name, sizeof(row->name), cur->name);
        if (elapsed_ns > 0) {
            const struct switch_thread_sample *prev =
                (before && before_count) ? (const struct switch_thread_sample *)bsearch(
                                               cur, before, before_count, sizeof(*before), switch_thread_sample_cmp)
                                         : NULL;
            const uint64_t du = prev ? switch_thread_delta(cur->user_ns, prev->user_ns) : cur->user_ns;
            const uint64_t ds = prev ? switch_thread_delta(cur->sys_ns, prev->sys_ns) : cur->sys_ns;
            row->cpu_pct = 100.0 * (double)(du + ds) / (double)elapsed_ns;
        } else {
            row->cpu_pct = (double)cur->cpu_permille / 10.0;
        }
    }
    if (after_count > 1)
        qsort(out, after_count, sizeof(*out), switch_thread_row_cmp);
    return after_count;
}

static inline void switch_threads_format(FILE *stream, const struct switch_thread_row *rows, size_t count, size_t limit)
{
    const size_t shown = (limit && limit < count) ? limit : count;
    fprintf(stream, "%-6s %-16s %6s %9s %9s %9s %-2s %3s %-16s %s\n", "PID", "TID", "%CPU", "USER_MS", "SYS_MS",
            "TOT_MS", "ST", "PRI", "PROCESS", "THREAD");
    for (size_t i = 0; i < shown; ++i) {
        const struct switch_thread_row *r = &rows[i];
        fprintf(stream, "%-6d 0x%014llx %6.1f %9.1f %9.1f %9.1f %-2c %3d %-16s %s\n", r->pid,
                (unsigned long long)r->tid, r->cpu_pct, r->user_ms, r->sys_ms, r->total_ms, r->state, r->pri, r->proc,
                r->name[0] ? r->name : "-");
    }
}

#ifdef __cplusplus
}
#endif
