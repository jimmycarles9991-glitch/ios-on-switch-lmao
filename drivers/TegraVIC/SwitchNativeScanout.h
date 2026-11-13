// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchNativeSurface.h"

struct SwitchNativeScanout {
    SwitchNativeSurfaceView view = {};
    switch_gpu_cpu_lease *lease = nullptr;
    switch_gpu_backing_info allocation = {};
    uint64_t pitchOffset = 0;
};

// turns: a picture TegraVIC recorded turned (SwitchVICTurn.h) too, for window A's column scan; view.turn says which.
int switch_native_scanout_acquire(OSObject *surface, SwitchNativeScanout *scanout, bool turns = false);
int switch_native_scanout_pitch(OSObject *surface, SwitchNativeScanout *scanout);
// _wait: sleeps for a CPU writer, for an engine reader, which orders behind writer promises on the GPU; _cpu: also until
// a promised job is submitted, for the CPU's own read (switch_gpu_backing_snapshot_read_cpu).
int switch_native_scanout_pitch_wait(OSObject *surface, SwitchNativeScanout *scanout);
int switch_native_scanout_pitch_cpu(OSObject *surface, SwitchNativeScanout *scanout);
void switch_native_scanout_release(SwitchNativeScanout *scanout);
// The display latched (or dropped) the frame the scanout shows: a recorded turn's request signals go now.
void switch_native_scanout_deliver(SwitchNativeScanout *scanout);

// What a scanout of the picture is ordered after: its producer, and a recorded turn's request waits. At most capacity.
inline uint32_t switch_native_scanout_dependencies(const SwitchNativeScanout &scanout, switch_gpu_backing_token *tokens,
                                                   uint32_t capacity)
{
    uint32_t count = 0;
    if (scanout.view.producer.engine && count < capacity)
        tokens[count++] = scanout.view.producer;
    for (uint32_t i = 0; i < scanout.view.waitCount && count < capacity; ++i)
        if (scanout.view.waits[i].engine)
            tokens[count++] = scanout.view.waits[i];
    return count;
}
