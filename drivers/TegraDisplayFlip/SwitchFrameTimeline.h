// SPDX-License-Identifier: BSD-2-Clause-Patent
// The frame timeline, debug kernels only: every point a compositor frame passes on its way to the panel is a kdebug
// tracepoint (KDBG_CODE(DBG_DRIVERS, DBG_DRVGRAPHICS, SWITCH_KDBG_*), free while tracing is off), and NativeLegacyDisplay
// logs one "SwitchFrame" line for each present that came 1.5 refresh periods or more after the one before, with the
// frame's points (tools/frames reads them from a pulled log archive). The GPU driver hands the display what it saw of
// the compositor's submissions for the frame (tegra_gm20b_frame_take). C and C++.
#pragma once
#include <stdint.h>

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define SWITCH_FRAME_TIMELINE 1
#else
#define SWITCH_FRAME_TIMELINE 0
#endif

enum {
    SWITCH_KDBG_VSYNC = 0x3f0,  // a vsync notification went to the render server (arg: frame ends)
    SWITCH_KDBG_SUBMIT,         // a user GPU submission was kicked (hardware channel, seqno, compositor)
    SWITCH_KDBG_GPU_DONE,       // a user channel's fence moved, seen by the interrupt pass (channel, seqno)
    SWITCH_KDBG_SWAP,           // accepted display ticket, CA swap ID, requested presentation, frame ends
    SWITCH_KDBG_PRESENT,        // GPU present sequence, display ticket, CA swap ID, take sequence
    SWITCH_KDBG_PRESENT_DONE,   // the GPU completed it (sequence, status)
    SWITCH_KDBG_FLIP,           // window A was asked to flip (surface)
    SWITCH_KDBG_LATCH,          // the flip latched at a frame end (surface, frames since the request)
    SWITCH_KDBG_FRAME_TICKET,   // latched display ticket, CA swap ID, take sequence, GPU present sequence
};

#define SWITCH_KDBG_CODE(code) ((6u << 24) | (18u << 16) | ((uint32_t)(code) << 2))

// The compositor's GPU submissions since the previous take; times in nanoseconds of mach absolute time.
struct SwitchFrameGpu {
    uint64_t firstSubmit, lastSubmit;  // 0: none
    uint32_t submits;                  // the compositor's
    uint32_t others;                   // every other process's
    uint64_t execNs;                   // CPU time in the compositor's submissions (kernel side)
    // Every process's new-surface work: BoImports, VmBind ops (map and unmap), BoAllocs, BoFrees, their kernel CPU time,
    // and the bytes the imports cleaned from the CPU caches and the time that took.
    uint32_t imports, binds, allocs, frees;
    uint64_t importNs, bindNs, allocNs, cleanNs, cleanBytes;
};

#ifdef __cplusplus
extern "C" {
#endif
// Provided by the GPU driver, weak in NativeLegacyDisplay: what it saw since the last call, then reset. Any thread.
void tegra_gm20b_frame_take(struct SwitchFrameGpu *out);
// Provided by XNU (kernel/switch/sched/render.c), weak and empty in the drivers that call them: Core Animation's display
// thread from each vsync to its swap. The display reports each vsync (its stamp and refresh period, mach) and, on the
// swapping thread, each swap; the GPU driver each compositor submission, on the submitting thread.
void switch_sched_render_vsync(uint64_t stamp, uint64_t period);
void switch_sched_render_submitted(void);
void switch_sched_render_swapped(void);
#ifdef __cplusplus
}
#endif
