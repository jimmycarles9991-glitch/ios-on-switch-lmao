// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch frame stalls (kernel/switch/diag/stall_frame.c, debug kernels): when the screen stops presenting in the middle
 * of an animation, the threads the next frame waits on, as they are then. A release kernel's switch_stall_frame does
 * nothing.
 */
#ifndef SWITCH_STALL_H
#define SWITCH_STALL_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <kern/kern_types.h>

__BEGIN_DECLS

/* The display, at a frame end: no present since last_present (mach time) though presents came every frame before it.
 * Once per stall. */
void switch_stall_frame(uint64_t last_present);

#if (DEVELOPMENT || DEBUG) && \
    ((defined(SWITCH_STATS_FRAME) && SWITCH_STATS_FRAME) || (defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED))
/* sched/stats.c: a task's new role (RunningBoard's focal app and SpringBoard), and backboardd naming Core Animation's
 * render-server thread. */
void switch_stall_frame_role(task_t task, int role);
void switch_stall_frame_render_server(thread_t thread);
#else
static inline void switch_stall_frame_role(__unused task_t task, __unused int role)
{}

static inline void switch_stall_frame_render_server(__unused thread_t thread)
{}
#endif

#if DEVELOPMENT || DEBUG
/* diag/watch.c: a kernel path that can block on something its caller cannot see (the GPU driver's BoImport wire). If
 * `thread` is still blocked `ns` after _begin, one "SwitchStall: <what>: ..." line names its wait and whom that wait is
 * for, as the UI stall lines do. _end before the path returns (it waits for a line being written); NULL is a watch that
 * could not be armed. */
struct switch_stall_watch;
struct switch_stall_watch *switch_stall_watch_begin(thread_t thread, uint64_t ns, const char *what);
void switch_stall_watch_end(struct switch_stall_watch *watch);
#else
struct switch_stall_watch;

static inline struct switch_stall_watch *switch_stall_watch_begin(__unused thread_t thread, __unused uint64_t ns,
                                                                  __unused const char *what)
{
    return NULL;
}

static inline void switch_stall_watch_end(__unused struct switch_stall_watch *watch)
{}
#endif

__END_DECLS

#endif /* SWITCH_STALL_H */
