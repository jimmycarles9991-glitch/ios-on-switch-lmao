// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "TegraVICSurface.h"

struct SwitchVICEvents;

// The backing holds `extent` bytes laid out pitch linear (its layout, or the GPU's legacy record of its first page).
bool switch_vic_pitch_backing(switch_gpu_backing *backing, uint64_t extent);

int switch_vic_transform(const TegraVICSurface *source, const TegraVICSurface *destination, const TegraVICPlan *plan,
                         uint32_t chromaSite, void (*completion)(void *, int), void *context,
                         SwitchVICEvents *events = nullptr);
