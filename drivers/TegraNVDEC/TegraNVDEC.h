// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraNVDEC: the engine (TegraNVDEC.cpp), its SMMU address space and buffers (TegraNVDECMemory.cpp), the user
// client (TegraNVDECUserClient.cpp).
//
// On unless boot-arg switch-no-nvdec: N0 host1x, N1 SMMU, power and falcon, N2 syncpoint proof of life, N3 the
// TegraNVDEC service and its user client.
#ifndef TEGRA_NVDEC_H
#define TEGRA_NVDEC_H
#include <stddef.h>
#include <stdint.h>
#include "TegraNVDECABI.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

// Hardware probes (SwitchNVDECProbeABI.h): debug kernels only, so a release kernel has none of their code or strings.
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define SWITCH_NVDEC_PROBES 1
#else
#define SWITCH_NVDEC_PROBES 0
#endif

struct switch_dma_buffer;
struct switch_gpu_backing;
struct SwitchNVDECCompletion;

// One buffer NVDEC reads or writes: any RAM pages, mapped at `iova` in NVDEC's address space, 64 KiB granular, zeroed
// and clean in the caches when allocated.
struct TegraNVDECBlock {
    uint64_t iova, size;
    uint8_t *va;
    switch_dma_pages pages;  // the memory a client maps, and its page list (VIC maps a picture's)
    switch_gpu_backing *backing;
};

// Resolves a submit's buffer handle; every user client supplies its own.
typedef bool (*TegraNVDECResolve)(void *context, uint32_t handle, TegraNVDECBlock *block);

extern "C" {
// NVDEC's address space (N1): its tables, the firmware and setup slots identity-mapped, the swgroups translated. 0, or
// -1 with one line in `line`. Detach puts the swgroups back in bypass (sleep, a failed bring-up); attach undoes it.
int tegra_nvdec_memory_init(const switch_dma_buffer *firmware, const switch_dma_buffer *setup, char *line, size_t size);
int tegra_nvdec_memory_attach(char *line, size_t size);
void tegra_nvdec_memory_detach(void);
void tegra_nvdec_memory_release(void);  // a failed bring-up gives the tables back (swgroups detached first)
// -12 when the wired-memory ceiling or the address window has no room.
int tegra_nvdec_buffer_alloc(uint64_t size, TegraNVDECBlock *block);
void tegra_nvdec_buffer_free(TegraNVDECBlock *block);  // only once no job can touch it
void tegra_nvdec_memory_usage(uint64_t *limit, uint64_t *free);

// Checks a submit (TegraNVDECCheck.h), cleans its CPU-written ranges, copies its picture setup into kernel memory
// and queues it on host1x channel 0. Returns 0 and the job's fence, or a negative errno-style value. A queue already
// holding SwitchNVDECQueue::kSlots jobs makes it sleep for room first (*waitedForRoom). `completion` (or null) runs
// once from the job's retirement (SwitchNVDECQueue.h).
int tegra_nvdec_run(const TegraNVDECSubmit *submit, TegraNVDECResolve resolve, void *context,
                    const SwitchNVDECCompletion *completion, uint32_t *fence, bool *waitedForRoom, const char **why);
#if SWITCH_NVDEC_PROBES
// tegra_nvdec_run with the display-buffer probe's check (SwitchNVDECDisplayProbe.h) in place of tegra_nvdec_check.
int switch_nvdec_run_display_probe(const TegraNVDECSubmit *submit, TegraNVDECResolve resolve, void *context,
                                   const SwitchNVDECCompletion *completion, uint32_t *fence, bool *waitedForRoom,
                                   const char **why);
#endif
// Sleeps until the fence is reached (syncpoint interrupt), bounded by the engine's own GP timer per queued job. Past
// that bound the job is cancelled with one line and its wait returns -5 (TegraNVDECCancel.cpp).
int tegra_nvdec_wait(uint32_t fence);
bool tegra_nvdec_ready(void);
// The bring-up has ended: 1 when NVDEC decodes (N3 published), 0 when it does not or `timeoutMs` passed first.
int tegra_nvdec_await(uint32_t timeoutMs);

uint32_t tegra_nvdec_codecs(void);  // bit n: TEGRA_NVDEC_CODEC_* n, accepted by Submit
int tegra_nvdec_publish(void);      // N3
}

#endif
