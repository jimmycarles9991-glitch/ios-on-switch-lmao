// SPDX-License-Identifier: GPL-2.0-only
/*
 * Which changes the kernel makes on Data before launchd (bsd/kern/kern_exec.c: launchd's overrides, the setup's
 * preferences, cache drops, markers, a panic record) reach the card before launchd starts. A REDONE change is made
 * again by the next boot's same step from the kernel's own state before anything reads it, so a power cut that loses
 * it costs one redo. Every other change is DURABLE: something may read it before the next step could redo it
 * (launchd's overrides, managed and setup preferences), or nothing redoes it (seeds, edits of the user's own plists,
 * a panic record). A boot syncs from its first durable change on: that change and every later write go out
 * synchronously, each directory sync after it runs, and so does the volume's at the end. Free of kernel types so
 * kernel/tests can drive it.
 */
#ifndef SWITCH_BOOT_SYNC_H
#define SWITCH_BOOT_SYNC_H

#include <stdbool.h>
#include <stddef.h>

enum switch_boot_change {
    SWITCH_BOOT_DURABLE,
    SWITCH_BOOT_REDONE,
};

struct switch_boot_sync {
    unsigned durable; /* durable changes made */
#if DEVELOPMENT || DEBUG
    char first[128]; /* the first one's path, cut to fit, for the step's debug line */
#endif
};

/* A change about to be made to `path` (a file written or removed, a directory made): whether it goes out
 * synchronously. A durable change counts before it is made, so it is synced itself. */
static inline bool switch_boot_sync_change(struct switch_boot_sync *sync, enum switch_boot_change change,
                                           const char *path)
{
    if (change == SWITCH_BOOT_DURABLE && !sync->durable++) {
#if DEVELOPMENT || DEBUG
        size_t i = 0;
        for (; path[i] && i < sizeof(sync->first) - 1; ++i) {
            sync->first[i] = path[i];
        }
        sync->first[i] = '\0';
#else
        (void)path;
#endif
    }
    return sync->durable != 0;
}

/* Whether a directory sync, or the volume's, runs now. */
static inline bool switch_boot_sync_due(const struct switch_boot_sync *sync)
{
    return sync->durable != 0;
}

/* The step's end, given the volume sync and the release of a saved panic's RAM copy (SwitchPanicSave.c). */
struct switch_boot_finish {
    void *context;
    int (*sync_volume)(void *context);
    void (*release_panic)(void *context, int error); /* keeps the RAM copy for the next boot unless error is 0 */
    bool panic_written;                              /* a saved panic's file was written this boot */
    int panic_error;                                 /* its write's error */
};

/*
 * The volume sync runs once a durable change was made, after a failed step too: what it changed before failing is
 * made durable as on any other boot. A saved panic's RAM copy goes only after that sync, which makes its file's name
 * durable: a panic or reset before it leaves the copy for the next boot. Returns the step's error, else the sync's.
 */
static inline int switch_boot_sync_finish(const struct switch_boot_sync *sync, int error,
                                          const struct switch_boot_finish *finish)
{
    const int synced = switch_boot_sync_due(sync) ? finish->sync_volume(finish->context) : 0;
    if (finish->panic_written) {
        finish->release_panic(finish->context, finish->panic_error ? finish->panic_error : synced);
    }
    return error ? error : synced;
}

#endif /* SWITCH_BOOT_SYNC_H */
