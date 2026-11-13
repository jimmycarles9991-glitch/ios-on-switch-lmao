// SPDX-License-Identifier: MIT
// Plane copies on the GPU's copy engine for other kernel drivers (TegraVIC's bounce between a plane at any pitch and one at
// the 256-byte pitch VIC addresses): no CPU touches the pixels and none waits for the copy. TegraGM20BCopy.cpp.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tegra_gm20b_ce_plane {
	void *memory;    /* an IOMemoryDescriptor, prepare()d by the caller until `done` runs */
	uint64_t offset; /* the plane's first byte in it, 16-byte aligned */
	/* bytes between rows; a block-linear IOSurface is read or written as its rows, this ignored */
	uint32_t pitch;
};

#define TEGRA_GM20B_CE_NO_SYNCPT 0xffffffffu

/* A host1x syncpoint threshold the copy waits for on the GPU before it starts (VIC's job that wrote the source) */
struct tegra_gm20b_ce_wait {
	uint32_t syncpt; /* TEGRA_GM20B_CE_NO_SYNCPT: none */
	uint32_t threshold;
};

struct tegra_gm20b_ce_done {
	/* host1x syncpoint the GPU host increments twice (nvgpu's per release) once the copy is in DRAM, or
	 * TEGRA_GM20B_CE_NO_SYNCPT */
	uint32_t syncpt;
	/* then called once on a thread call (thread context: it may submit host1x work; it must not sleep for long), with 0 or
	 * the copy channel's error; null: none */
	void (*fn)(void *arg, int status);
	void *arg;
};

struct switch_gpu_backing;
struct switch_gpu_event_submission;
struct switch_gpu_cpu_lease;
struct switch_gpu_backing_guard;

struct switch_gpu_copy_ownership {
	struct switch_gpu_cpu_lease *const *leases;
	uint32_t lease_count;
	const struct switch_gpu_backing_guard *guards;
	uint32_t guard_count;
};

/* Copies `rows` rows of `row_bytes` bytes from `src` into `dst` at dst->pitch, after the GPU's writes to either IOSurface
 * (semaphore acquires on the GPU) and `wait` (null: none); the destination then counts the copy among its GPU writers.
 * A block-linear surface (TegraGM20BLayout.h) on either side, never both, is a whole surface: offset 0, row_bytes its
 * width in bytes, rows its height; its texels are read or written as its rows. Thread context; may sleep briefly for the
 * GPU mappings, never for the GPU. Backing offsets identify each plane's first byte; the descriptor pages are checked
 * against that retained allocation. Mappings persist between copies. Returns 0, or an error with nothing queued: a
 * failed admission consumes no callback or signal reservation. */
int tegra_gm20b_ce_copy_plane_checked(
	const struct tegra_gm20b_ce_plane *src, const struct tegra_gm20b_ce_plane *dst,
	uint32_t row_bytes, uint32_t rows, const struct tegra_gm20b_ce_wait *wait,
	const struct tegra_gm20b_ce_done *done, struct switch_gpu_backing *src_backing,
	uint64_t src_backing_offset, struct switch_gpu_backing *dst_backing,
	uint64_t dst_backing_offset, const struct switch_gpu_copy_ownership *ownership,
	const struct switch_gpu_event_submission *events);

#ifdef __cplusplus
}
#endif
