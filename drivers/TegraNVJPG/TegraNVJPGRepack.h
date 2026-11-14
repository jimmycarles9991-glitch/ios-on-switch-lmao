// SPDX-License-Identifier: BSD-2-Clause-Patent
// A '422f' or '444f' decode (TegraNVJPGJpeg.h): NVJPG writes the file's planar Y'CbCr into the service's own buffer, and
// VIC re-packs it into the destination behind NVJPG's fence (SwitchVICDecodeOutput.h).
#pragma once
#include <stdint.h>
#include "TegraNVJPG.h"
#include "TegraNVJPGRequest.h"
#include "../TegraVIC/TegraVICSurface.h"

// Maps the buffer (grown to the plan's repackBytes) and the scan's `bits` (the sink page listed after them) into
// NVJPG's windows, runs the decode with VIC's re-pack queued to wait for its fence on the hardware, and returns once
// VIC has written the destination: 0, or the decode's or VIC's error (`why` set for a refusal before any engine work).
// Engine thread; takes the job lock.
int tegra_nvjpg_repack(TegraNVJPGJob *engine, const TegraNVJPGPlan *plan, const TegraNVJPGDecodeRequest *request,
                       const TegraVICSurface *destination, const uint64_t *bits, uint32_t bitPages,
                       nvjpg_dec_status *status, uint64_t *busyUs, const char **why);
