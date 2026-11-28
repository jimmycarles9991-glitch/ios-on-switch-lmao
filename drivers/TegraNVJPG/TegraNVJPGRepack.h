// SPDX-License-Identifier: BSD-2-Clause-Patent
// Hardware DMA re-pack behind NVJPG's syncpoint fence: VIC for '422f'/'444f' (SwitchVICDecodeOutput.h), and GM20B Copy
// Engine for pitch/block-linear layout mismatches (TegraGM20BCopy.h).
#pragma once
#include <stdint.h>
#include "TegraNVJPG.h"
#include "TegraNVJPGRequest.h"
#include "../TegraVIC/TegraVICSurface.h"

#include "../TegraVIC/SwitchVICSurfaceAccess.h"

struct switch_gpu_backing;

// A re-packed decode's state, from tegra_nvjpg_repack_start to tegra_nvjpg_repack_end. NVJPG's start queues the
// re-pack (its follow) and VIC's or the copy engine's completion ends it after the start has returned, so it lives in
// the decode's job, never on a stack. `queued` and `why` are the follow's, written on the engine thread inside
// tegra_nvjpg_start; `pending` and `status` the completions' (atomics). A queued re-pack's end is its signal alone,
// the completions' last use of the run: its owner learns of it there, never by reading the run.
struct TegraNVJPGRepackRun {
    const TegraNVJPGPlan *plan;
    const TegraNVJPGDecodeRequest *request;
    const TegraVICSurface *destination;
    SwitchVICSurfaceAccess access;
    TegraNVJPGSignal signal;  // given when the re-pack ends
    const char *why;
    uint32_t vicFence;  // VIC's job behind the decode, when the re-pack is VIC's (switch_vic_job_background)
    uint32_t pending;
    int status;
    bool ce, queued;
    bool guardClosed;  // HW Test's copy-guard probe: the copy engine's re-pack queued with its guard already 0
};

// Starts the decode from the scan at IOVA `bitstream` with VIC's or the copy engine's re-pack queued behind its
// fence. 0: NVJPG holds the job (job lock held; tegra_nvjpg_finish after `ended`, then the lock's release), and the
// re-pack's end gives `repacked` (in either order: the re-pack waits on NVJPG's syncpoint in the hardware, but the two
// ends come from different work loops). Otherwise the decode never reached the engine (`why` for a refusal before any
// engine work), or its fence could not be armed (the job cancelled, a re-pack it queued still to end), and only
// tegra_nvjpg_repack_end follows once a queued re-pack has signalled. `guardClosed`: a debug kernel's copy-guard probe
// (image.nvjpg_copy_guard), whose copies launch with their guard 0, as a cancelled decode's do. Engine thread.
int tegra_nvjpg_repack_start(TegraNVJPGRepackRun *run, TegraNVJPGJob *engine, const TegraNVJPGPlan *plan,
                             const TegraNVJPGDecodeRequest *request, const TegraVICSurface *destination,
                             switch_gpu_backing *backing, uint64_t bitstream, const TegraNVJPGSignal *ended,
                             const TegraNVJPGSignal *repacked, bool guardClosed, const char **why);
// Once NVJPG's part is finished (`ret`: its result, or the start's error) and a queued re-pack has signalled: the
// decode's result, the destination's access released. Engine thread.
int tegra_nvjpg_repack_end(TegraNVJPGRepackRun *run, int ret);
