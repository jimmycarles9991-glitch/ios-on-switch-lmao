// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraNVENC: the engine (TegraNVENC.cpp), its SMMU address space and buffers (TegraNVENCMemory.cpp) and the
// user client (TegraNVENCUserClient.cpp).
//
// On by default; the stages (TegraNVENC.cpp) are E0 host1x, E1 SMMU, power and falcon, E2 syncpoint proof of life, E3
// the TegraNVENC service and its user client. switch-no-nvdec turns it off.
#ifndef TEGRA_NVENC_H
#define TEGRA_NVENC_H
#include <stddef.h>
#include <stdint.h>
#include "TegraNVENCABI.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

struct switch_dma_buffer;

// One buffer NVENC reads or writes: any RAM pages, mapped at `iova` in NVENC's address space, 64 KiB granular, zeroed
// and clean in the caches when allocated.
struct TegraNVENCBlock {
    uint64_t iova, size;
    uint8_t *va;
    switch_dma_pages pages;  // the memory a client maps, and its page list
};

// Resolves a submit's buffer handle; every user client supplies its own.
typedef bool (*TegraNVENCResolve)(void *context, uint32_t handle, TegraNVENCBlock *block);

extern "C" {
// NVENC's address space (E1): its tables, the firmware, setup slots and scratch identity-mapped, the swgroup translated.
// 0, or -1 with one line in `line`. Detach puts the swgroup back in bypass (sleep); attach undoes it.
int tegra_nvenc_memory_init(const switch_dma_buffer *firmware, const switch_dma_buffer *setup,
                            const switch_dma_buffer *scratch, char *line, size_t size);
int tegra_nvenc_memory_attach(char *line, size_t size);
void tegra_nvenc_memory_detach(void);
void tegra_nvenc_memory_release(void);  // a failed bring-up gives the tables back (swgroup detached first)
// -12 when the wired-memory ceiling or the address window has no room.
int tegra_nvenc_buffer_alloc(uint64_t size, TegraNVENCBlock *block);
void tegra_nvenc_buffer_free(TegraNVENCBlock *block);  // only once no job can touch it
void tegra_nvenc_memory_usage(uint64_t *limit, uint64_t *free);

// Checks a submit (TegraNVENCCheck.h), cleans its CPU-written ranges, copies its setup into kernel memory, writes 0
// in the address methods of features it leaves off and queues it on host1x channel 0. Returns 0 and the job's fence,
// or a negative errno-style value.
int tegra_nvenc_run(const TegraNVENCSubmit *submit, TegraNVENCResolve resolve, void *context, uint32_t *fence,
                    const char **why);
// Sleeps until the fence is reached (syncpoint interrupt), bounded per queued job. Past that bound: a one-line panic.
int tegra_nvenc_wait(uint32_t fence);
bool tegra_nvenc_ready(void);

int tegra_nvenc_publish(void);  // E3
}

#endif
