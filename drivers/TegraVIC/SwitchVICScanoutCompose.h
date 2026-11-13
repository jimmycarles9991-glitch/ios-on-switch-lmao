// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchNativeScanout.h"
#include "TegraVICCompose.h"

// One slot of a composite: the picture it reads (plane bases are offsets within that capture's allocation, which the
// caller keeps through admission) and where it lands; slot.source is filled in here.
struct SwitchVICComposeSource {
    const SwitchNativeScanout *scanout;
    TegraVICImage image;
    TegraVICComposeSlot slot;
};

constexpr uint32_t kSwitchVICComposeSources = 3;  // two-plane sources one job's methods hold (TegraVICCompose.h)

// Composes sources[0] (bottom) .. sources[count - 1] into `destination`, laid out as `layout` (format, size, pitch,
// rows, block linear, with base 0), within `target`. Returns once VIC has the job; `output` is the destination with
// VIC's producer token, as switch_vic_scanout_convert's. -95: the engine refuses the job.
int switch_vic_scanout_compose(const SwitchVICComposeSource *sources, uint32_t count, switch_gpu_backing *destination,
                               const TegraVICImage &layout, const TegraVICRect &target, SwitchNativeScanout *output);
