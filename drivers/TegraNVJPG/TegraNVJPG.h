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
// and the destination surface. Below the DRAM aperture (0x80000000), so they never meet the 1:1 kernel buffers. The
// input window holds every queue slot's scan buffer at once.
enum {
    TEGRA_NVJPG_WINDOW_IN = 0,
    TEGRA_NVJPG_WINDOW_OUT = 1,
    TEGRA_NVJPG_WINDOWS = 2
};

constexpr uint64_t kTegraNVJPGWindowBase[TEGRA_NVJPG_WINDOWS] = {0x10000000ULL, 0x20000000ULL};
constexpr uint64_t kTegraNVJPGWindowBytes[TEGRA_NVJPG_WINDOWS] = {256ULL << 20, 1536ULL << 20};
constexpr uint64_t kTegraNVJPGOutputBytes = 256ULL << 20;

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

// Work queued behind a decode before the CPU sleeps on it: `queue` gets NVJPG's syncpoint and the decode's fence.
struct TegraNVJPGFollow {
    void (*queue)(void *context, uint32_t syncpt, uint32_t fence);
    void *context;
};

extern "C" {
// Engine (TegraNVJPG.cpp).
bool tegra_nvjpg_ready(void);
const TegraNVJPGTraits *tegra_nvjpg_traits(void);
void tegra_nvjpg_lock(void);  // held across tegra_nvjpg_run only: sleep waits for the job on the engine
void tegra_nvjpg_unlock(void);
// `count` pages, then `sinkCount` entries of the kernel's sink page, at the lowest free IOVA of `window`; 0 when the
// window has no room. Each mapping lives as long as its buffer and is unmapped (`mapped` = count + sinkCount) only once
// the engine can no longer touch it. Neither takes the job lock: a backing's retirement unmaps while a decode runs.
uint64_t tegra_nvjpg_pages_map(uint32_t window, const uint64_t *pages, uint32_t count, uint32_t sinkCount,
                               bool writable);
void tegra_nvjpg_pages_unmap(uint32_t window, uint64_t address, uint32_t mapped);
// Copies the setup to the kernel's slot, queues the job's methods on host1x channel 3 behind an OP_DONE increment of
// NVJPG's syncpoint, runs `follow` (another engine's job queued to wait for that increment on the hardware) and sleeps
// until the increment's interrupt (bounded: past the bound the job is cancelled, -5). Fills the firmware's status. Job
// lock held.
int tegra_nvjpg_run(const TegraNVJPGJob *job, nvjpg_dec_status *status, uint64_t *busyUs,
                    const TegraNVJPGFollow *follow = nullptr);

// Service (TegraNVJPGUserClient.cpp): J3.
int tegra_nvjpg_publish(void);
}

#endif
