// SPDX-License-Identifier: BSD-2-Clause-Patent
// A '422f' or '444f' decode (TegraNVJPGJpeg.h): NVJPG writes the file's planar Y'CbCr into the service's own buffer, and
// VIC re-packs it into the destination behind NVJPG's fence (SwitchVICDecodeOutput.h).
#pragma once
#include <stdint.h>
#include "TegraNVJPG.h"
#include "TegraNVJPGRequest.h"
#include "../TegraVIC/TegraVICSurface.h"

// Keeps the grown repack buffer mapped through its backing, runs the decode from the request's bitstream IOVA with
// VIC's re-pack queued to wait for its fence on the hardware, and returns once
// VIC has written the destination: 0, or the decode's or VIC's error (`why` set for a refusal before any engine work).
// Engine thread; takes the job lock.
int tegra_nvjpg_repack(TegraNVJPGJob *engine, const TegraNVJPGPlan *plan, const TegraNVJPGDecodeRequest *request,
                       const TegraVICSurface *destination, uint64_t bitstream, nvjpg_dec_status *status,
                       uint64_t *busyUs, const char **why);
