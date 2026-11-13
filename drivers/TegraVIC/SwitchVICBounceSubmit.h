// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraVICSurface.h"
struct SwitchVICEvents;
int switch_vic_bounce_submit(const TegraVICSurface *source, const TegraVICSurface *destination,
                             const TegraVICPlan *plan, uint32_t chromaSite, void (*completion)(void *, int),
                             void *context, SwitchVICEvents *events);
