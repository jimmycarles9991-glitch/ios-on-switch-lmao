// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchNativeSurface.h"

struct SwitchNativeScanout {
    SwitchNativeSurfaceView view = {};
    switch_gpu_cpu_lease *lease = nullptr;
    switch_gpu_backing_info allocation = {};
    switch_gpu_backing_token promises[SWITCH_GPU_BACKING_PROMISES] = {};
    uint32_t promiseCount = 0;
    uint64_t pitchOffset = 0;
};

// turns: a picture TegraVIC recorded turned (SwitchVICTurn.h) too, for window A's column scan; view.turn says which.
// -2: no picture to scan (none, or one retired until the next publication: the surface shows its rows); -95: one the
// caller may not scan as it is; -16: a publication or a writer holds the surface now; SWITCH_GPU_BACKING_STALE: the
// picture was replaced under this acquire.
int switch_native_scanout_acquire(OSObject *surface, SwitchNativeScanout *scanout, bool turns = false);
int switch_native_scanout_pitch(OSObject *surface, SwitchNativeScanout *scanout);
// The same with the backing the caller already holds for the surface: no IOSurface cache lookup.
int switch_native_scanout_pitch_held(OSObject *surface, switch_gpu_backing *held, SwitchNativeScanout *scanout);
// _wait: sleeps for a CPU writer, for an engine reader, which orders behind writer promises on the GPU; _cpu: also until
// a promised job is submitted, for the CPU's own read (switch_gpu_backing_snapshot_read_cpu).
int switch_native_scanout_pitch_wait(OSObject *surface, SwitchNativeScanout *scanout);
int switch_native_scanout_pitch_cpu(OSObject *surface, SwitchNativeScanout *scanout);
void switch_native_scanout_release(SwitchNativeScanout *scanout);
// The display latched (or dropped) the frame the scanout shows: a recorded turn's request signals go now.
void switch_native_scanout_deliver(SwitchNativeScanout *scanout);

inline bool switch_native_scanout_same_promises(const SwitchNativeScanout &a, const SwitchNativeScanout &b)
{
    if (a.promiseCount != b.promiseCount)
        return false;
    for (uint32_t i = 0; i < a.promiseCount; ++i)
        if (a.promises[i].engine != b.promises[i].engine || a.promises[i].queue != b.promises[i].queue ||
            a.promises[i].reset_generation != b.promises[i].reset_generation ||
            a.promises[i].value != b.promises[i].value)
            return false;
    return true;
}

// What a scanout of the picture is ordered after: its producer, a recorded turn's request waits, and any writer
// promises captured atomically with the snapshot. At most capacity.
inline uint32_t switch_native_scanout_dependencies(const SwitchNativeScanout &scanout, switch_gpu_backing_token *tokens,
                                                   uint32_t capacity)
{
    uint32_t count = 0;
    if (scanout.view.producer.engine && count < capacity)
        tokens[count++] = scanout.view.producer;
    for (uint32_t i = 0; i < scanout.view.waitCount && count < capacity; ++i)
        if (scanout.view.waits[i].engine)
            tokens[count++] = scanout.view.waits[i];
    for (uint32_t i = 0; i < scanout.promiseCount && count < capacity; ++i)
        if (scanout.promises[i].engine)
            tokens[count++] = scanout.promises[i];
    return count;
}
