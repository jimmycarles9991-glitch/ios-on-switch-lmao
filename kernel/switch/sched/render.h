// SPDX-License-Identifier: GPL-2.0-only
/*
 * The render thread's frame (render.c, debug kernels): where Core Animation's display thread spends the time from the
 * vsync that wakes it to its swap, on a core, blocked, or waiting for one. Its own dispatches split the time: between
 * two of them the thread ran for the CPU time it gained, was off the core from then until it was made runnable again
 * (blocked), and waited for a core after that. Plain data, so kernel/tests runs it on the host. Times are mach ticks.
 *
 * Core Animation's display timer can fire just before the frame end it renders for: the thread then wakes in the frame
 * before, already swapped, and the vsync arrives while it runs, often with no dispatch after it. So the first dispatch
 * after a swap that lies within half / 8 of the predicted next vsync (about 1 ms at 60 Hz) opens that frame's window
 * itself, the vsync predicted from the display's latest stamp (pending); the display's own stamp replaces it when its
 * count arrives, at a dispatch or at the swap.
 */
#ifndef SWITCH_SCHED_RENDER_H
#define SWITCH_SCHED_RENDER_H

#include <stdbool.h>
#include <stdint.h>

struct switch_render_window {
    uint64_t vsyncs;     /* the display's vsync count the window began at */
    uint64_t vsync;      /* that vsync's stamp */
    uint64_t woke, cpu0; /* the thread's first dispatch after it, and its CPU time then */
    uint64_t on, cpu;    /* its latest dispatch, and its CPU time then */
    uint64_t blocked, runnable, longest;
    uint32_t blocks;
    /* its first GPU submission in the window, and the CPU and blocked time to it */
    uint64_t first, first_cpu, first_blocked;
    uint32_t first_blocks;
    bool swapped;
    bool pending; /* opened before its vsync: `vsyncs` and `vsync` are predicted until the display's count reaches it */
};

static inline void switch_render_open(struct switch_render_window *w, uint64_t vsyncs, uint64_t vsync, uint64_t now,
                                      uint64_t cpu, bool pending)
{
    __builtin_memset(w, 0, sizeof(*w));
    w->vsyncs = vsyncs;
    w->vsync = vsync;
    w->woke = w->on = now;
    w->cpu0 = w->cpu = cpu;
    w->pending = pending;
}

/* The display reached count `vsyncs` with stamp `vsync`: a pending window's predicted vsync takes the stamp. */
static inline void switch_render_seen(struct switch_render_window *w, uint64_t vsyncs, uint64_t vsync)
{
    if (w->pending && vsyncs == w->vsyncs) {
        w->pending = false;
        w->vsync = vsync;
    }
}

/*
 * A dispatch at `now` with the thread's CPU time `cpu` and the time it was made runnable (`made`, 0: not known);
 * `vsyncs` and `vsync` are the display's count and latest stamp, `half` half its period (0 before it reported one).
 * A new frame opens a window here; otherwise the gap since the previous dispatch is accounted, and true is returned
 * when its block is the window's longest so far (the caller notes where it was). A block of `min_block` or more
 * counts as one; less is a preemption's requeue.
 */
static inline bool switch_render_dispatched(struct switch_render_window *w, uint64_t vsyncs, uint64_t vsync,
                                            uint64_t now, uint64_t cpu, uint64_t made, uint64_t min_block,
                                            uint64_t half)
{
    const uint64_t early = half / 8;
    const uint64_t next = vsync + 2 * half;
    bool longest = false;
    bool before = false;

    switch_render_seen(w, vsyncs, vsync);
    before = w->pending && vsyncs + 1 == w->vsyncs;
    /* Tight band around the predicted vsync: tolerates vsync jitter without swallowing late wakes, post-swap work, or
     * stale stamps (frame ends masked, display idle). */
    if (!before && w->swapped && early != 0 && now >= next - early && now < next + early) {
        switch_render_open(w, vsyncs + 1, next, now, cpu, true);
        return false;
    }
    if (!before && vsyncs != w->vsyncs) {
        switch_render_open(w, vsyncs, vsync, now, cpu, false);
        return false;
    }
    if (w->on != 0 && cpu >= w->cpu && made != 0) {
        const uint64_t off = w->on + (cpu - w->cpu);

        if (made > off) {
            const uint64_t blocked = made - off;

            w->blocked += blocked;
            if (blocked >= min_block) {
                w->blocks++;
            }
            if (blocked > w->longest) {
                w->longest = blocked;
                longest = true;
            }
        }
        if (now > made) {
            w->runnable += now - made;
        }
    }
    w->on = now;
    w->cpu = cpu;
    return longest;
}

/*
 * The thread swapped at `now`, the display at count `vsyncs` with latest stamp `vsync`. False when the window has no
 * swap to take (none open, or swapped already). Otherwise the window is swapped, and *late says whether the swap came
 * half a period (`half`) or more after the window's vsync; a swap before a predicted vsync is on time.
 */
static inline bool switch_render_swap(struct switch_render_window *w, uint64_t vsyncs, uint64_t vsync, uint64_t now,
                                      uint64_t half, bool *late)
{
    switch_render_seen(w, vsyncs, vsync);
    if (w->woke == 0 || w->swapped) {
        return false;
    }
    w->swapped = true;
    *late = now >= w->vsync && now - w->vsync >= half;
    return true;
}

/* The thread submitted GPU work at `now`, with CPU time `cpu`: the window's first submission is kept. */
static inline void switch_render_submitted(struct switch_render_window *w, uint64_t now, uint64_t cpu)
{
    if (w->woke == 0 || w->first != 0 || w->swapped) {
        return;
    }
    w->first = now;
    w->first_cpu = cpu - w->cpu0;
    w->first_blocked = w->blocked;
    w->first_blocks = w->blocks;
}

#endif /* SWITCH_SCHED_RENDER_H */
