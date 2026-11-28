// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels' decode counts (TegraNVJPGStats.cpp): the 5 s unified-log line, and per process what NVJPG decoded,
// failed and refused, which HW Test's image.jpeg_* read through the route call (TegraNVJPGUserClient.cpp).
#ifndef TEGRA_NVJPG_STATS_H
#define TEGRA_NVJPG_STATS_H
#include <stdint.h>
#include "TegraNVJPG.h"

#if TEGRA_NVJPG_PROBES
// What one process asked of NVJPG: `formats` bit n for each TEGRA_NVJPG_OUT_n decoded, `asked[n]` the requests for
// TEGRA_NVJPG_OUT_n, refused and failed ones too, `routes[n]` the decodes routed by TEGRA_NVJPG_ROUTE_n (its
// TEGRA_NVJPG_ROUTE_COUNT), `cancelled` its jobs the engine cancelled (past their bound, or on a falcon error). HW Test
// reads the fields at fixed offsets: new ones go last.
struct TegraNVJPGRouteCounts {
    uint64_t decodes, formats, failed, refused, asked[TEGRA_NVJPG_OUT_COUNT], outputMaps, routes[4], cancelled;
};

bool tegra_nvjpg_stats_start(void);
void switch_nvjpg_count_mapping(int pid);
// A request refused before any engine work; `format` TEGRA_NVJPG_OUT_COUNT when no destination was read.
void tegra_nvjpg_count_refused(int pid, uint32_t format);
// A job the engine ran.
void tegra_nvjpg_count_job(int pid, uint32_t format, bool failed, uint64_t engineUs, uint64_t cpuUs, uint64_t pixels,
                           uint64_t bytes);
void tegra_nvjpg_counts_of(int pid, TegraNVJPGRouteCounts *out);
void tegra_nvjpg_count_route(int pid, uint32_t route);
void tegra_nvjpg_count_cancelled(int pid);
// TegraNVJPG.cpp's: the jobs cancelled since boot. Only the service's engine thread runs jobs, so the change across one
// job's run is that job's.
uint64_t tegra_nvjpg_cancellations(void);
#endif
#if TEGRA_NVJPG_STATS
// The window's line once 5 s have passed with work in them.
void tegra_nvjpg_stats_line(void);
// A decode's route (TEGRA_NVJPG_ROUTE_*): what GM20B answered for the destination (`held` block-linear, its row bytes
// and y_log2), its backing pages, and what NVJPG was told. Every decode into a block-linear destination to os_log,
// the first of each route otherwise to the console. Engine thread only.
void tegra_nvjpg_note_route(uint32_t route, bool held, uint32_t rowBytes, uint32_t yLog2, uint32_t pages,
                            const nvjpg_dec_drv_pic_setup_s *setup, uint32_t width, uint32_t height);
// TegraNVJPG.cpp's, taken and zeroed: completed engine waits. Takes no lock: a client's refused decode calls it too.
void tegra_nvjpg_lock_counts(uint64_t *waitsUnlocked);
#endif

#endif
