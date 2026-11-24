// SPDX-License-Identifier: GPL-2.0-only
/*
 * Page-ins in a window (pagein.c, debug kernels), by process and by file: the file pager's reads (how many, the bytes
 * their clusters asked for, the faulting thread's time in the read) and the waits for busy pages a file read is filling
 * (how many, how long). A file system that reads asynchronously returns from the read at once and the faulting thread
 * then waits for the busy page, so a process's time lost to page-ins is its time reading plus its time waiting. A file
 * is keyed by its vnode for the window and named by a read; one first met by a wait stays unnamed until a read of it.
 * Plain data and text, so kernel/tests runs it on the host; the including file declares snprintf. Times are mach ticks.
 */
#ifndef SWITCH_DIAG_PAGEIN_H
#define SWITCH_DIAG_PAGEIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_PAGEIN_ROWS 64u /* a power of two */
#define SWITCH_PAGEIN_NAME 32u
#define SWITCH_PAGEIN_TOP  6u
/* A key lives within this many rows of its hash: a full table answers a new key's miss after as many probes. */
#define SWITCH_PAGEIN_PROBES 8u

struct switch_pagein_row {
    uint64_t key; /* a process: its pid + 1; a file: its vnode. 0: empty */
    char name[SWITCH_PAGEIN_NAME];
    uint32_t reads, waits;
    uint64_t bytes, read, wait;
};

struct switch_pagein_window {
    struct switch_pagein_row process[SWITCH_PAGEIN_ROWS], file[SWITCH_PAGEIN_ROWS];
    uint32_t reads, waits, others, untracked; /* others: busy-page waits that were not for a file read */
    uint64_t bytes, read, wait, other_wait;
    uint64_t longest; /* the longest single wait for a file read, its process and file */
    int32_t longest_pid;
    char longest_process[SWITCH_PAGEIN_NAME], longest_file[SWITCH_PAGEIN_NAME];
};

/* XNU's own counts over the window: its page-ins, and the phantom cache's ghosts: `found` pages paged in again while
 * their eviction was still in its table (evicted, then read again), `added` evictions entered, `pages` the table's
 * reach (the last this many evicted pages). */
struct switch_pagein_xnu {
    uint64_t pageins, found, added;
    uint32_t pages;
};

static inline struct switch_pagein_row *switch_pagein_find(struct switch_pagein_row *rows, uint64_t key, bool insert)
{
    uint32_t i = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 58) & (SWITCH_PAGEIN_ROWS - 1);

    for (uint32_t probe = 0; probe < SWITCH_PAGEIN_PROBES; probe++, i = (i + 1) & (SWITCH_PAGEIN_ROWS - 1)) {
        if (rows[i].key == key) {
            return &rows[i];
        }
        if (rows[i].key == 0) {
            if (!insert) {
                return NULL;
            }
            __builtin_memset(&rows[i], 0, sizeof(rows[i]));
            rows[i].key = key;
            return &rows[i];
        }
    }
    return NULL;
}

static inline void switch_pagein_name(char *to, const char *from)
{
    uint32_t i = 0;

    for (; from != NULL && i + 1 < SWITCH_PAGEIN_NAME && from[i] != '\0'; i++) {
        to[i] = from[i];
    }
    to[i] = '\0';
}

static inline struct switch_pagein_row *switch_pagein_process(struct switch_pagein_window *w, int32_t pid,
                                                              const char *process)
{
    struct switch_pagein_row *p = switch_pagein_find(w->process, (uint64_t)(uint32_t)pid + 1, true);

    if (p != NULL && p->name[0] == '\0') {
        switch_pagein_name(p->name, process);
    }
    return p;
}

/* A file pager read by process `pid` (`process` its name) of `bytes` from the file `vnode` (`file` its name, or NULL
 * when the caller has none yet: then false while the window has no name for it, and the caller asks again with the
 * name), the thread spending `read` in it. */
static inline bool switch_pagein_note_read(struct switch_pagein_window *w, int32_t pid, const char *process,
                                           uint64_t vnode, const char *file, uint64_t bytes, uint64_t read)
{
    struct switch_pagein_row *f = switch_pagein_find(w->file, vnode, file != NULL);
    struct switch_pagein_row *p = NULL;

    if ((f == NULL || f->name[0] == '\0') && file == NULL) {
        return false;
    }
    if (f != NULL && f->name[0] == '\0') {
        switch_pagein_name(f->name, file);
    }
    p = switch_pagein_process(w, pid, process);
    if (f == NULL || p == NULL) {
        w->untracked++;
    }
    if (f != NULL) {
        f->reads++;
        f->bytes += bytes;
        f->read += read;
    }
    if (p != NULL) {
        p->reads++;
        p->bytes += bytes;
        p->read += read;
    }
    w->reads++;
    w->bytes += bytes;
    w->read += read;
    return true;
}

/* A busy-page wait by process `pid` of `wait`: for a read of the file `vnode`, or (vnode 0) for anything else (a page
 * being written back, wired, decompressed or read by another pager, or still busy after its read while the fault
 * finishes, code-signing validation included). */
static inline void switch_pagein_note_wait(struct switch_pagein_window *w, int32_t pid, const char *process,
                                           uint64_t vnode, uint64_t wait)
{
    struct switch_pagein_row *f = NULL;
    struct switch_pagein_row *p = NULL;

    if (vnode == 0) {
        w->others++;
        w->other_wait += wait;
        return;
    }
    f = switch_pagein_find(w->file, vnode, true);
    p = switch_pagein_process(w, pid, process);
    if (f == NULL || p == NULL) {
        w->untracked++;
    }
    if (f != NULL) {
        f->waits++;
        f->wait += wait;
    }
    if (p != NULL) {
        p->waits++;
        p->wait += wait;
    }
    w->waits++;
    w->wait += wait;
    if (wait > w->longest) {
        w->longest = wait;
        w->longest_pid = pid;
        switch_pagein_name(w->longest_process, process);
        switch_pagein_name(w->longest_file, f != NULL ? f->name : NULL);
    }
}

static inline size_t switch_pagein_put(size_t size, size_t at, int n)
{
    if (n < 0 || at >= size) {
        return at;
    }
    return at + (size_t)n < size ? at + (size_t)n : size - 1;
}

static inline unsigned long long switch_pagein_ms(uint64_t ticks, uint32_t numer, uint32_t denom)
{
    return (unsigned long long)(ticks * numer / denom / 1000000u);
}

/* The `top` rows by time reading and waiting, most first, each "name reads/KiB/ms reading/waits/ms waiting" (pids
 * after process names; a file no read named yet is "(unnamed)"). */
static inline size_t switch_pagein_top(char *out, size_t size, size_t at, const struct switch_pagein_row *rows,
                                       bool process, uint32_t numer, uint32_t denom)
{
    bool shown[SWITCH_PAGEIN_ROWS] = {false};

    for (uint32_t k = 0; k < SWITCH_PAGEIN_TOP; k++) {
        uint32_t best = SWITCH_PAGEIN_ROWS;

        for (uint32_t i = 0; i < SWITCH_PAGEIN_ROWS; i++) {
            if (!shown[i] && rows[i].key != 0 &&
                (best == SWITCH_PAGEIN_ROWS || rows[i].read + rows[i].wait > rows[best].read + rows[best].wait)) {
                best = i;
            }
        }
        if (best == SWITCH_PAGEIN_ROWS) {
            break;
        }
        shown[best] = true;
        const struct switch_pagein_row *r = &rows[best];
        at = switch_pagein_put(
            size, at,
            snprintf(out + at, size - at, "%s%s", k ? ", " : " ", r->name[0] != '\0' ? r->name : "(unnamed)"));
        if (process) {
            at = switch_pagein_put(size, at, snprintf(out + at, size - at, "[%d]", (int)(r->key - 1)));
        }
        at = switch_pagein_put(size, at,
                               snprintf(out + at, size - at, " %u/%llu/%llu/%u/%llu", r->reads,
                                        (unsigned long long)(r->bytes >> 10), switch_pagein_ms(r->read, numer, denom),
                                        r->waits, switch_pagein_ms(r->wait, numer, denom)));
    }
    return at;
}

/* `xnu` may be NULL (not read). "1200 reads, 96000 KiB asked, 40 ms reading; 1500 waits for pages being read,
 * 2300 ms (longest 116 ms: SpringBoard[400] on dyld_shared_cache_arm64e); 12 other busy-page waits, 3 ms; untracked
 * 0; XNU page-ins 3400, 120 evicted and paged in again (ghosts found, 900 added, the last 25600 evictions held); by
 * process (reads/KiB asked/ms reading/waits/ms waiting): SpringBoard[400] 190/15200/12/210/1044, ...". The rows come
 * last: a cut line loses only its last rows. The files have a line of their own (switch_pagein_format_files). */
static inline size_t switch_pagein_format(char *out, size_t size, const struct switch_pagein_window *w,
                                          const struct switch_pagein_xnu *xnu, uint32_t numer, uint32_t denom)
{
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    at = switch_pagein_put(size, at,
                           snprintf(out, size,
                                    "%u reads, %llu KiB asked, %llu ms reading; %u waits for pages being read, "
                                    "%llu ms",
                                    w->reads, (unsigned long long)(w->bytes >> 10),
                                    switch_pagein_ms(w->read, numer, denom), w->waits,
                                    switch_pagein_ms(w->wait, numer, denom)));
    if (w->waits != 0) {
        at = switch_pagein_put(size, at,
                               snprintf(out + at, size - at, " (longest %llu ms: %s[%d] on %s)",
                                        switch_pagein_ms(w->longest, numer, denom), w->longest_process,
                                        (int)w->longest_pid,
                                        w->longest_file[0] != '\0' ? w->longest_file : "(unnamed)"));
    }
    at = switch_pagein_put(size, at,
                           snprintf(out + at, size - at, "; %u other busy-page waits, %llu ms; untracked %u", w->others,
                                    switch_pagein_ms(w->other_wait, numer, denom), w->untracked));
    if (xnu != NULL) {
        at = switch_pagein_put(
            size, at,
            snprintf(out + at, size - at,
                     "; XNU page-ins %llu, %llu evicted and paged in again (ghosts found, %llu added, "
                     "the last %u evictions held)",
                     (unsigned long long)xnu->pageins, (unsigned long long)xnu->found, (unsigned long long)xnu->added,
                     xnu->pages));
    }
    at = switch_pagein_put(
        size, at, snprintf(out + at, size - at, "; by process (reads/KiB asked/ms reading/waits/ms waiting):"));
    return switch_pagein_top(out, size, at, w->process, true, numer, denom);
}

/* The files with the most time reading plus waiting: "dyld_shared_cache_arm64e 900/72000/30/1100/1800, ..." */
static inline size_t switch_pagein_format_files(char *out, size_t size, const struct switch_pagein_window *w,
                                                uint32_t numer, uint32_t denom)
{
    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    return switch_pagein_top(out, size, 0, w->file, false, numer, denom);
}

#endif /* SWITCH_DIAG_PAGEIN_H */
