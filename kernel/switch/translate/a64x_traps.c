// SPDX-License-Identifier: GPL-2.0-only
/*
 * The translator's traps by class and by process (<switch/a64x_traps.h>), debug kernels: a64x_emulate_user notes each
 * trap it handled; the scheduling report takes a window of them.
 */
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/systm.h>
#include <kern/cpu_data.h>
#include <kern/cpu_number.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <pexpert/arm64/board_config.h>
#include <switch/a64x.h>
#include <switch/a64x_traps.h>

#include "../../../translate/arm64e/a64x/a64x.h"

#if CONFIG_A64X && (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO

static struct switch_a64x_ring a64x_rings[MAX_CPUS] __attribute__((aligned(64)));
static uint64_t a64x_class_traps[A64X_NCLASSES];
static uint64_t a64x_class_seen[A64X_NCLASSES];

void switch_a64x_trap_noted(unsigned int cls, bool patched)
{
    struct switch_a64x_note note = {.pid = proc_getpid(current_proc()), .cls = (uint8_t)cls, .patched = patched};

    strlcpy(note.name, current_proc()->p_comm, sizeof(note.name));
    if (cls < A64X_NCLASSES) {
        OSIncrementAtomic64((SInt64 *)&a64x_class_traps[cls]);
    }
    disable_preemption();
    switch_a64x_ring_push(&a64x_rings[cpu_number()], &note);
    enable_preemption();
}

/* The report's thread: "LSE_CAS 8, PAC_LDRA 4; by process ..." for the traps since the last call, 0 when none. */
size_t switch_a64x_trap_window(char *out, size_t size)
{
    struct switch_a64x_window w = {0};
    uint64_t total = 0;
    size_t at = 0;

    for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
        switch_a64x_ring_drain(&a64x_rings[cpu], &w);
    }
    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    at = switch_a64x_put(size, at, snprintf(out, size, "by class"));
    for (unsigned int c = 0; c < A64X_NCLASSES; c++) {
        const uint64_t now = os_atomic_load(&a64x_class_traps[c], relaxed);
        const uint64_t n = now - a64x_class_seen[c];

        a64x_class_seen[c] = now;
        if (n != 0) {
            at = switch_a64x_put(size, at,
                                 snprintf(out + at, size - at, "%s %s %llu", total ? "," : "",
                                          a64x_class_name((enum a64x_class)c), (unsigned long long)n));
            total += n;
        }
    }
    if (total == 0 && w.count == 0 && w.dropped == 0) {
        out[0] = '\0';
        return 0;
    }
    at = switch_a64x_put(size, at, snprintf(out + at, size - at, "; "));
    return at + switch_a64x_window_format(out + at, size - at, &w);
}

#elif CONFIG_A64X && (DEVELOPMENT || DEBUG)

size_t switch_a64x_trap_window(char *out, size_t size);

size_t switch_a64x_trap_window(char *out, size_t size)
{
    if (size != 0) {
        out[0] = '\0';
    }
    return 0;
}

#endif
