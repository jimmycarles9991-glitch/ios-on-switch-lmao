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
};

struct switch_icon_cache_result {
    bool requested; /* the deploy named a GPU build the cache was not last dropped for */
    int dropped;    /* __cache_info__ files removed */
    int error;      /* what stopped it; the marker is then left as it was, so the next boot drops again */
    char build[13]; /* the named build's first characters */
};

void switch_icon_cache_drop(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result);

#endif /* SWITCH_ICON_CACHE_H */
