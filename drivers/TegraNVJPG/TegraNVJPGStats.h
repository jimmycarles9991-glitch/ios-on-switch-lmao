// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels' decode counts (TegraNVJPGStats.cpp): the 5 s unified-log line, and per process what NVJPG decoded,
// failed and refused, which HW Test's image.jpeg_* read through the route call (TegraNVJPGUserClient.cpp).
#ifndef TEGRA_NVJPG_STATS_H
#define TEGRA_NVJPG_STATS_H
#include <stdint.h>
#include "TegraNVJPG.h"

#if TEGRA_NVJPG_STATS
// What one process asked of NVJPG: `formats` bit n for each TEGRA_NVJPG_OUT_n decoded, `asked[n]` the requests for
// TEGRA_NVJPG_OUT_n, refused and failed ones too.
struct TegraNVJPGRouteCounts {
    uint64_t decodes, formats, failed, refused, asked[TEGRA_NVJPG_OUT_COUNT];
};

bool tegra_nvjpg_stats_start(void);
// A request refused before any engine work; `format` TEGRA_NVJPG_OUT_COUNT when no destination was read.
void tegra_nvjpg_count_refused(int pid, uint32_t format);
// A job the engine ran.
void tegra_nvjpg_count_job(int pid, uint32_t format, bool failed, uint64_t engineUs, uint64_t cpuUs, uint64_t pixels,
                           uint64_t bytes);
void tegra_nvjpg_counts_of(int pid, TegraNVJPGRouteCounts *out);
// The window's line once 5 s have passed with work in them.
void tegra_nvjpg_stats_line(void);
// TegraNVJPG.cpp's, taken and zeroed: the jobs whose engine wait ran with gStateLock free, and the longest a
// cancellation or an idle gate call waited to take gStateLock. Takes no lock: a client's refused decode calls it too.
void tegra_nvjpg_lock_counts(uint64_t *waitsUnlocked, uint64_t *stateWaitMaxUs);
#endif

#endif
