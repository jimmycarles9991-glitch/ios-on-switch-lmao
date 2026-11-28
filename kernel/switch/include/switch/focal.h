// SPDX-License-Identifier: GPL-2.0-only
/*
 * The focal apps a debug watcher follows (sched/busy.c, diag/stall_frame.c, recorder/watch.c). RunningBoard makes
 * several processes TASK_FOREGROUND_APPLICATION at once: on 25 Nov a Capture press made ScreenshotServicesService
 * focal over HW Test, and its leaving ended the watch on HW Test for 15 minutes. Each watcher keeps one place per focal
 * app, keyed by its task, and this says which place a role change uses. A place holds a reference on the task's main
 * thread, which keeps the task alive, so a task pointer cannot be reused while it is a key. Plain data, so kernel/tests
 * runs it on the host.
 */
#ifndef SWITCH_FOCAL_H
#define SWITCH_FOCAL_H

#include <stdint.h>

#define SWITCH_FOCAL_PLACES 3u

struct switch_focal {
    uint64_t owner[SWITCH_FOCAL_PLACES]; /* the focal task's address, 0: free */
    uint64_t since[SWITCH_FOCAL_PLACES]; /* when it became focal */
};

/* The place `owner` takes on becoming focal at `now`: its own, else a free one, else the one focal the longest, whose
 * owner *evicted then names (0 otherwise). */
static inline unsigned switch_focal_enter(struct switch_focal *f, uint64_t owner, uint64_t now, uint64_t *evicted)
{
    unsigned place = SWITCH_FOCAL_PLACES;

    *evicted = 0;
    for (unsigned i = 0; i < SWITCH_FOCAL_PLACES; i++) {
        if (f->owner[i] == owner) {
            return i;
        }
        if (f->owner[i] == 0 && place == SWITCH_FOCAL_PLACES) {
            place = i;
        }
    }
    if (place == SWITCH_FOCAL_PLACES) {
        place = 0;
        for (unsigned i = 1; i < SWITCH_FOCAL_PLACES; i++) {
            if (f->since[i] < f->since[place]) {
                place = i;
            }
        }
        *evicted = f->owner[place];
    }
    f->owner[place] = owner;
    f->since[place] = now;
    return place;
}

/* The place `owner` held, now free, or SWITCH_FOCAL_PLACES when it held none: the other focal apps keep theirs. */
static inline unsigned switch_focal_leave(struct switch_focal *f, uint64_t owner)
{
    for (unsigned i = 0; i < SWITCH_FOCAL_PLACES; i++) {
        if (f->owner[i] == owner) {
            f->owner[i] = 0;
            f->since[i] = 0;
            return i;
        }
    }
    return SWITCH_FOCAL_PLACES;
}

#endif
