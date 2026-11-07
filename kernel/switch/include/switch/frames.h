// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch frame diagnostics (kernel/switch/diag/frames.c): the commit cadence of the render server's clients and the
 * delivery of touch reports, for the display's stats and interaction lines. Debug kernels only; a release kernel
 * compiles the hooks to nothing and the drivers' calls to empty answers.
 */
#ifndef SWITCH_FRAMES_H
#define SWITCH_FRAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <ipc/ipc_port.h>
#include <kern/task.h>

__BEGIN_DECLS

#if DEVELOPMENT || DEBUG
/* ipc_kmsg_send, under the destination port's lock: a message (its id) to the render server, `own` when the render
 * server sent it itself (not a client's), and the render server's own message to a port of another task. */
void switch_frames_render_message(task_t sender, mach_msg_id_t id, bool own);
void switch_frames_render_server_sent(ipc_port_t port);
#else
static inline void switch_frames_render_message(__unused task_t sender, __unused mach_msg_id_t id, __unused bool own)
{}

static inline void switch_frames_render_server_sent(__unused ipc_port_t port)
{}
#endif

/* SwitchTouch, for each report it posts on a lit screen: its interrupt time (mach), whether a contact is down, and
 * whether one went down or lifted in it (diag/latency.h's SWITCH_LATENCY_BEGAN_BIT, SWITCH_LATENCY_ENDED_BIT). */
void switch_frames_touch(uint64_t stamp, bool down, unsigned int changes);

/* The display: when the current run of touches began and when a contact was last down (mach; 0: no touch yet). A run
 * goes on while each report comes less than SWITCH_FRAMES_RUN_GAP_MS after the last one with a contact down. */
#define SWITCH_FRAMES_RUN_GAP_MS 1000
void switch_frames_touch_activity(uint64_t *began, uint64_t *last_down);

/* The display's lines: the window's clients by commits and the touch deliveries, then the counts start over.
 * `interaction` picks the accumulators of the touch interaction window, otherwise those of the stats window. */
size_t switch_frames_line(bool interaction, char *out, size_t size);

/* The display, for each present of a swap: the vsync it was drawn for (0: not known), its submission and its latch
 * (mach). With the touch reports and the render server's messages, these time each stage of a touch's way to the
 * panel; the latency line (diag/latency.h) reports them since the last one and starts over. */
void switch_frames_present(uint64_t vsync, uint64_t submit, uint64_t latch);
size_t switch_frames_latency_line(char *out, size_t size);

__END_DECLS

#endif /* SWITCH_FRAMES_H */
