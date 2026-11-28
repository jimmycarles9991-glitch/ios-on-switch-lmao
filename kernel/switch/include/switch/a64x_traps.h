// SPDX-License-Identifier: GPL-2.0-only
/*
 * Which processes iOS 27's translator traps for, and what for (translate/a64x_traps.c, debug kernels): each trap
 * a64x_emulate_user handles, an instruction emulated or a JIT word patched, leaves a note in its CPU's ring, and the
 * 10 s scheduling report (sched/cpu.c) drains every ring into one window. A ring has one writer, its CPU with preemption
 * disabled (a trap is taken in the thread's own context, and no interrupt handler traps for the translator), and one
 * reader, the report; a full ring drops the note and counts it, so the window's processes are the first
 * SWITCH_A64X_RING traps of each CPU while its totals stay exact. Plain C, free of kernel types, so kernel/tests runs
 * it on the host; the including file declares snprintf.
 */
#ifndef SWITCH_A64X_TRAPS_H
#define SWITCH_A64X_TRAPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_A64X_RING      256u
#define SWITCH_A64X_PROCESSES 16u
#define SWITCH_A64X_TOP       4u
#define SWITCH_A64X_NAME      17u

struct switch_a64x_note {
    int32_t pid;
    uint8_t cls;
    bool patched;
    char name[SWITCH_A64X_NAME];
};

struct switch_a64x_ring {
    uint32_t head;         /* the writer's */
    uint32_t dropped;      /* the writer's */
    uint32_t tail;         /* the reader's */
    uint32_t dropped_seen; /* the reader's */
    struct switch_a64x_note notes[SWITCH_A64X_RING];
};

struct switch_a64x_process {
    int32_t pid;
    char name[SWITCH_A64X_NAME];
    uint32_t emulated, patched;
};

struct switch_a64x_window {
    uint32_t dropped, unlisted; /* notes no ring kept; notes of processes past SWITCH_A64X_PROCESSES */
    uint32_t count;
    struct switch_a64x_process processes[SWITCH_A64X_PROCESSES];
};

static inline void switch_a64x_ring_push(struct switch_a64x_ring *r, const struct switch_a64x_note *note)
{
    const uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);

    if (head - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) >= SWITCH_A64X_RING) {
        __atomic_store_n(&r->dropped, __atomic_load_n(&r->dropped, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
        return;
    }
    r->notes[head % SWITCH_A64X_RING] = *note;
    __atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);
}

static inline void switch_a64x_window_add(struct switch_a64x_window *w, const struct switch_a64x_note *note)
{
    struct switch_a64x_process *p = NULL;

    for (uint32_t i = 0; i < w->count; i++) {
        if (w->processes[i].pid == note->pid) {
            p = &w->processes[i];
            break;
        }
    }
    if (p == NULL) {
        if (w->count == SWITCH_A64X_PROCESSES) {
            w->unlisted++;
            return;
        }
        p = &w->processes[w->count++];
        p->pid = note->pid;
        __builtin_memcpy(p->name, note->name, sizeof(p->name));
        p->name[SWITCH_A64X_NAME - 1] = '\0';
        p->emulated = p->patched = 0;
    }
    if (note->patched) {
        p->patched++;
    } else {
        p->emulated++;
    }
}

static inline void switch_a64x_ring_drain(struct switch_a64x_ring *r, struct switch_a64x_window *w)
{
    const uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    const uint32_t dropped = __atomic_load_n(&r->dropped, __ATOMIC_RELAXED);
    uint32_t tail = r->tail;

    for (; tail != head; tail++) {
        switch_a64x_window_add(w, &r->notes[tail % SWITCH_A64X_RING]);
    }
    __atomic_store_n(&r->tail, tail, __ATOMIC_RELEASE);
    w->dropped += dropped - r->dropped_seen;
    r->dropped_seen = dropped;
}

static inline size_t switch_a64x_put(size_t size, size_t at, int n)
{
    if (n < 0 || at >= size) {
        return at;
    }
    return at + (size_t)n < size ? at + (size_t)n : size - 1;
}

/* "by process jsc[412] 10 (3 patched), ...; 2 processes more, 0 notes dropped": the SWITCH_A64X_TOP with the most. */
static inline size_t switch_a64x_window_format(char *out, size_t size, const struct switch_a64x_window *w)
{
    bool shown[SWITCH_A64X_PROCESSES] = {false};
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    at = switch_a64x_put(size, at, snprintf(out + at, size - at, "by process"));
    for (uint32_t n = 0; n < SWITCH_A64X_TOP && n < w->count; n++) {
        uint32_t best = SWITCH_A64X_PROCESSES;

        for (uint32_t i = 0; i < w->count; i++) {
            const uint32_t traps = w->processes[i].emulated + w->processes[i].patched;

            if (!shown[i] &&
                (best == SWITCH_A64X_PROCESSES || traps > w->processes[best].emulated + w->processes[best].patched)) {
                best = i;
            }
        }
        shown[best] = true;
        const struct switch_a64x_process *p = &w->processes[best];
        at = switch_a64x_put(size, at,
                             snprintf(out + at, size - at, "%s %s[%d] %u (%u patched)", n ? "," : "", p->name, p->pid,
                                      p->emulated + p->patched, p->patched));
    }
    return switch_a64x_put(size, at,
                           snprintf(out + at, size - at, "; %u processes more, %u notes dropped",
                                    w->count > SWITCH_A64X_TOP ? w->count - SWITCH_A64X_TOP + w->unlisted : w->unlisted,
                                    w->dropped));
}

#endif /* SWITCH_A64X_TRAPS_H */
