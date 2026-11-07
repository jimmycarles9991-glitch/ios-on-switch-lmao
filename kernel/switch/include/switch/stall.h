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

#if DEVELOPMENT || DEBUG
/* sched/stats.c: a task's new role (RunningBoard's focal app and SpringBoard), and backboardd naming Core Animation's
 * render-server thread. */
void switch_stall_frame_role(task_t task, int role);
void switch_stall_frame_render_server(thread_t thread);
#endif

__END_DECLS

#endif /* SWITCH_STALL_H */
