// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraNVDEC: the engine (TegraNVDEC.cpp), its SMMU address space and buffers (TegraNVDECMemory.cpp), the user
// client (TegraNVDECUserClient.cpp) and the boot self-test (TegraNVDECSelfTest.cpp).
//
// On (stage 3) unless boot-arg switch-no-nvdec. Stage 1 brings NVDEC up (N0 host1x, N1 SMMU, power and falcon, N2
// syncpoint proof of life); 2 also decodes embedded pictures (N3); 3 also publishes the TegraNVDEC service and its user
// client (N4).
#ifndef TEGRA_NVDEC_H
#define TEGRA_NVDEC_H
#include <stddef.h>
#include <stdint.h>
#include "TegraNVDECABI.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

struct switch_dma_buffer;

// One buffer NVDEC reads or writes: any RAM pages, mapped at `iova` in NVDEC's address space, 64 KiB granular, zeroed
// and clean in the caches when allocated.
struct TegraNVDECBlock {
    uint64_t iova, size;
    uint8_t *va;
    switch_dma_pages pages;  // the memory a client maps, and its page list (VIC maps a picture's)
};

// Resolves a submit's buffer handle; the self-test and every user client supply their own.
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
void tegra_nvdec_cache_clean(const void *va, size_t size);  // dc cvac: the CPU's writes reach DRAM
void tegra_nvdec_cache_flush(const void *va, size_t size);  // dc civac: the next CPU read comes from DRAM

// Checks a submit (TegraNVDECCheck.h), cleans its CPU-written ranges, copies its picture setup into kernel memory
// and queues it on host1x channel 0. Returns 0 and the job's fence, or a negative errno-style value.
int tegra_nvdec_run(const TegraNVDECSubmit *submit, TegraNVDECResolve resolve, void *context, uint32_t *fence,
                    const char **why);
// Sleeps until the fence is reached (syncpoint interrupt), bounded by the engine's own GP timer per queued job. Past
// that bound: a one-line panic once published (N4); during the N3 self-test, -60 and the engine refuses work.
int tegra_nvdec_wait(uint32_t fence);
bool tegra_nvdec_ready(void);
// The userland clip check's verdict (TegraNVDECABI.h): claim (returns the state; *claimed for the one owner), the
// owner's report (a failure clears that codec's bit: VP9, HEVC, HEVC Main 10), a bounded wait while it runs, and the owner's release.
uint32_t tegra_nvdec_check_claim(const void *owner, bool *claimed);
int tegra_nvdec_check_report(const void *owner, bool vp9Ok, bool hevcOk, bool hevc10Ok);
uint32_t tegra_nvdec_check_wait(void);
void tegra_nvdec_check_release(const void *owner);

// The bring-up has ended: 1 when NVDEC decodes (N4 published), 0 when it does not or `timeoutMs` passed first.
// TegraNVENC's E3 decodes its picture here.
int tegra_nvdec_await(uint32_t timeoutMs);

int tegra_nvdec_self_test(void);       // N3 H.264: NVDEC itself
int tegra_nvdec_self_test_vp9(void);   // N3 VP9: decides the VP9 bit of tegra_nvdec_codecs()
int tegra_nvdec_self_test_hevc(void);  // N3 HEVC: decides the HEVC (and Main 10) bits
uint32_t tegra_nvdec_codecs(void);     // bit n: TEGRA_NVDEC_CODEC_* n proven at N3 and accepted by Submit
int tegra_nvdec_publish(void);         // N4
}

#endif
