// SPDX-License-Identifier: GPL-2.0-only
/*
 * IconServices' rendered icons let go once per GPU build a deploy names (kernel/switch/boot/icon_cache.c), for the
 * boot-time setup in bsd/kern/kern_exec.c before launchd. Free of kernel types so kernel/tests can drive it over a host
 * directory.
 */
#ifndef SWITCH_ICON_CACHE_H
#define SWITCH_ICON_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_ICON_CACHE_TRASH             "Switch/icon-cache-trash"
#define SWITCH_ICON_CACHE_KEEP              "Switch/icon-cache-keep"
#define SWITCH_ICON_CACHE_SWEEP_MAX_DEPTH   4u
#define SWITCH_ICON_CACHE_SWEEP_MAX_ENTRIES 65536u

/* The Data volume's files, by their paths below its root (/private/var). */
struct switch_icon_cache_ops {
    void *context;
    /* The whole file into buffer, *length bytes of at most size; ENOENT when there is none. */
    int (*read)(void *context, const char *path, char *buffer, size_t size, size_t *length);
    /* Whether the file holds exactly these bytes, as write leaves it. */
    bool (*holds)(void *context, const char *path, const char *bytes, size_t length);
    int (*write)(void *context, const char *path, const char *bytes, size_t length);
    /* ENOENT when there is no such file. */
    int (*unlink)(void *context, const char *path);
    /* visit(walk, name) for each subdirectory of path until one returns an error; ENOENT when path is none. */
    int (*folders)(void *context, const char *path, int (*visit)(void *walk, const char *name), void *walk);
    /* Create path as a root-owned directory with mode if absent, or verify existing root-owned directory. */
    int (*mkdir)(void *context, const char *path, unsigned int mode);
    /* Atomic same-volume rename without following symlinks; ENOENT when from is absent. */
    int (*rename)(void *context, const char *from, const char *to);
    /* True when path exists as a Data-volume directory without following symlinks. */
    bool (*exists)(void *context, const char *path);
    /* Boot session UUID string used to namespace renamed cache folders under SWITCH_ICON_CACHE_TRASH. */
    const char *session;
};

struct switch_icon_cache_result {
    bool requested;     /* the deploy named a GPU build the cache was not last dropped for */
    bool trash_pending; /* SWITCH_ICON_CACHE_TRASH is present and needs post-boot sweep */
    int dropped;        /* __cache_info__ files unlinked (21H461) or salt folders moved to trash (24B119) */
    int kept;           /* 24B119: salt folders kept for the build the marker named */
    int restored;       /* 24B119: salt folders the named build left in the keep, moved back into the cache */
    int error;          /* what stopped it; the marker is then left as it was, so the next boot drops again */
    char build[13];     /* the named build's first characters, the keep's folder name for it */
};

void switch_icon_cache_drop(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result);

enum switch_icon_cache_entry_kind {
    SWITCH_ICON_CACHE_ENTRY_MISSING = 0,
    SWITCH_ICON_CACHE_ENTRY_REG,
    SWITCH_ICON_CACHE_ENTRY_DIR,
    SWITCH_ICON_CACHE_ENTRY_SYMLINK,
    SWITCH_ICON_CACHE_ENTRY_FOREIGN,
    SWITCH_ICON_CACHE_ENTRY_OTHER,
};

struct switch_icon_cache_sweep_ops {
    void *context;
    /* Caller-owned buffer of (SWITCH_ICON_CACHE_SWEEP_MAX_DEPTH + 1) * scratch_stride bytes. */
    char *scratch;
    size_t scratch_stride;
    /* Pack up to *count NUL-terminated child names (excluding "." and "..") into names, closing the dir first. */
    int (*read_batch)(void *context, const char *path, char *names, size_t names_size, size_t *count);
    /* Classify path without following symlinks and verifying its mount matches the Data volume. */
    int (*inspect)(void *context, const char *path, enum switch_icon_cache_entry_kind *kind);
    int (*unlink)(void *context, const char *path);
    int (*rmdir)(void *context, const char *path);
};

struct switch_icon_cache_sweep_result {
    unsigned int files_removed;
    unsigned int dirs_removed;
    int error;
};

void switch_icon_cache_sweep(const struct switch_icon_cache_sweep_ops *ops,
                             struct switch_icon_cache_sweep_result *result);

enum {
    SWITCH_ICON_CACHE_STATE_PENDING = 1u << 0,
    SWITCH_ICON_CACHE_STATE_BOOT_DONE = 1u << 1,
    SWITCH_ICON_CACHE_STATE_STARTED = 1u << 2,
};

bool switch_icon_cache_state_step(uint32_t current, uint32_t event_bit, uint32_t *next);

void switch_icon_cache_sweep_arm(void *data_mount, bool trash_pending);
void switch_icon_cache_boot_complete(void);

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_ICON_CACHE_H */
