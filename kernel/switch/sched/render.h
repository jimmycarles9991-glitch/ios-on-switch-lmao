// SPDX-License-Identifier: GPL-2.0-only
/*
 * The render thread's frame (render.c, debug kernels): where Core Animation's display thread spends the time from the
 * vsync that wakes it to its swap, on a core, blocked, or waiting for one. Its own dispatches split the time: between
 * two of them the thread ran for the CPU time it gained, was off the core from then until it was made runnable again
 * (blocked), and waited for a core after that. Plain data, so kernel/tests runs it on the host. Times are mach ticks.
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
};

/*
 * A dispatch at `now` with the thread's CPU time `cpu` and the time it was made runnable (`made`, 0: not known). A vsync
 * since the window began (`vsyncs`, the display's count) starts a new one here. Otherwise the gap since the previous
 * dispatch is accounted, and true is returned when its block is the window's longest so far (the caller notes where it
 * was). A block of `min_block` or more counts as one; less is a preemption's requeue.
 */
static inline bool switch_render_dispatched(struct switch_render_window *w, uint64_t vsyncs, uint64_t vsync,
                                            uint64_t now, uint64_t cpu, uint64_t made, uint64_t min_block)
{
    bool longest = false;

    if (vsyncs != w->vsyncs) {
        __builtin_memset(w, 0, sizeof(*w));
        w->vsyncs = vsyncs;
        w->vsync = vsync;
        w->woke = now;
        w->cpu0 = cpu;
    } else if (w->on != 0 && cpu >= w->cpu && made != 0) {
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
