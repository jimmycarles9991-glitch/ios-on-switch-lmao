// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraNVJPG: the engine and its SMMU domain (TegraNVJPG.cpp), the JPEG parser and setup translation
// (TegraNVJPGJpeg.h) and the AppleJPEGDriver service (TegraNVJPGUserClient.cpp).
//
// On by default; no boot-arg. Stages (TegraNVJPG.cpp): J0 host1x and the kernel buffers, J1 SMMU domain, power, clock
// and falcon, J2 syncpoint proof of life by interrupt, J3 the AppleJPEGDriver service published.
#ifndef TEGRA_NVJPG_H
#define TEGRA_NVJPG_H
#include <stddef.h>
#include <stdint.h>
#include "TegraNVJPGJpeg.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

// The periodic decode line: debug kernels only, so a release kernel has none of its code or strings.
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define TEGRA_NVJPG_STATS 1
#else
#define TEGRA_NVJPG_STATS 0
#endif

// A physically contiguous kernel buffer below 4 GiB, mapped 1:1 in NVJPG's address space (IOVA = PA).
struct TegraNVJPGBuffer {
    uint64_t pa, size;
    uint8_t *va;
};

// The two windows of NVJPG's address space a job's memory is mapped into, page by page: the scan's bytes (read-only)
// and the destination surface. Below the DRAM aperture (0x80000000), so they never meet the 1:1 kernel buffers.
enum {
    TEGRA_NVJPG_WINDOW_IN = 0,
    TEGRA_NVJPG_WINDOW_OUT = 1,
    TEGRA_NVJPG_WINDOWS = 2
};

constexpr uint64_t kTegraNVJPGWindowBase[TEGRA_NVJPG_WINDOWS] = {0x10000000ULL, 0x20000000ULL};
constexpr uint64_t kTegraNVJPGWindowBytes[TEGRA_NVJPG_WINDOWS] = {64ULL << 20, 256ULL << 20};

// What the engine decodes right (TegraNVJPG.cpp kTraits); the service builds every job with it.
struct TegraNVJPGTraits {
    bool clipped;      // writes stay inside the picture (else a destination must hold the whole MCU grid)
    uint32_t formats;  // bit n: TEGRA_NVJPG_OUT_n decodes correctly
    uint32_t chroma;   // bit n: TEGRA_NVJPG_CHROMA_n input sampling decodes correctly
    uint32_t scales;   // bit n: downscale by 2^n proven (bit 0 always)
    bool greyRgb;      // a grey file decodes to RGB (output chroma mode 0)
};

// One decode: a complete setup, the scan's bytes and the output planes as IOVAs (each a 256-byte multiple).
struct TegraNVJPGJob {
    nvjpg_dec_drv_pic_setup_s setup;
    uint64_t bitstream;
    uint64_t plane[3];
    uint32_t planes;
    uint64_t pixels;            // the grid's pixels: the wait bound
    uint32_t selector, target;  // the AppleJPEGDriver selector it came through, its destination's format code
};

extern "C" {
// Engine (TegraNVJPG.cpp).
bool tegra_nvjpg_ready(void);
const TegraNVJPGTraits *tegra_nvjpg_traits(void);
int tegra_nvjpg_buffer(uint64_t size, TegraNVJPGBuffer *out);  // from the kernel arena; never freed
void tegra_nvjpg_lock(void);  // held across a job's map, run and unmap (one job at a time)
void tegra_nvjpg_unlock(void);
// Maps `count` 4 KiB pages at the start of a window (the rest of the window unmapped); returns the window's IOVA, or 0
// if they do not fit. `writable` false maps them read-only for the engine. Job lock held.
uint64_t tegra_nvjpg_window_map(uint32_t window, const uint64_t *pages, uint32_t count, bool writable);
void tegra_nvjpg_window_clear(uint32_t window);
// Copies the setup to the kernel's slot, queues the job's methods on host1x channel 0 behind an OP_DONE increment of
// NVJPG's syncpoint and sleeps until that increment's interrupt (bounded: past the bound a one-line panic). Fills the
// firmware's status. Job lock held.
int tegra_nvjpg_run(const TegraNVJPGJob *job, nvjpg_dec_status *status, uint64_t *busyUs);

// Service (TegraNVJPGUserClient.cpp): J3.
int tegra_nvjpg_publish(void);
}

#endif
