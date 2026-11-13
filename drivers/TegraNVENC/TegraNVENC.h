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
#include <kern/task.h>
#include "TegraNVENCABI.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

struct switch_dma_buffer;

// One buffer NVENC reads or writes: any RAM pages, mapped at `iova` in NVENC's address space, 64 KiB granular, zeroed
// and clean in the caches when allocated. Its backing is what another engine writing it (VIC, an input picture) is
// ordered by; the pages and the mapping go when the last reference to it does.
struct TegraNVENCBlock {
    uint64_t iova, size;
    uint8_t *va;
    switch_dma_pages pages;  // the memory a client maps, and its page list
    switch_gpu_backing *backing;
};

// Another engine's syncpoint threshold a job's stream waits for on the hardware before its methods, and the job there
// that wrote what this one reads, if any (its failure is this job's: TegraNVENCInputs.h).
struct TegraNVENCAfter {
    uint32_t syncpt, threshold;
    switch_gpu_backing_token producer;
};

// A job that reads another engine's memory under SwitchGpuBacking's ownership (TegraNVENCDirect.cpp): with the engine
// lock held, begin opens the use with the job's fence and gives the producers' thresholds to wait for (at most
// TEGRA_NVENC_ORDER_WAITS); end commits it once the job is queued, or abandons it.
enum {
    TEGRA_NVENC_ORDER_WAITS = 4
};

struct TegraNVENCOrder {
    int (*begin)(void *context, uint32_t fence, TegraNVENCAfter *waits, uint32_t *count);
    void (*end)(void *context, uint32_t fence, bool queued);
    void *context;
};

// Resolves a submit's buffer handle; every user client supplies its own.
typedef bool (*TegraNVENCResolve)(void *context, uint32_t handle, TegraNVENCBlock *block);

// The codecs Submit takes and GetInfo reports (bit n: TEGRA_NVENC_CODEC_* n). H.265 is IDR pictures only: its P
// pictures' reference fields are not established (nvidia/SOURCE-H265).
constexpr uint32_t kTegraNVENCCodecs = 1u << TEGRA_NVENC_CODEC_H264 | 1u << TEGRA_NVENC_CODEC_H265;

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
// Another engine's backing mapped read-only in NVENC's address space, cached by allocation: a handle held until no job
// can read it (release with tegra_nvenc_native_unmap), its address and size; null when there is no room.
void *tegra_nvenc_native_map(switch_gpu_backing *backing, uint64_t *iova, uint64_t *bytes);
void tegra_nvenc_native_unmap(void *handle);

// Checks a submit (TegraNVENCCheck.h): 0, or -22 with the reason.
int tegra_nvenc_check_submit(const TegraNVENCSubmit *submit, TegraNVENCResolve resolve, void *context,
                             const char **why);
// Checks a submit, cleans its CPU-written ranges, copies its setup into kernel memory, writes 0 in the address methods
// of features it leaves off and queues it on host1x channel 0, after `after` and `order`'s producers when given.
// Returns 0 and the job's fence, or a negative errno-style value.
int tegra_nvenc_run(const TegraNVENCSubmit *submit, TegraNVENCResolve resolve, void *context,
                    const TegraNVENCAfter *after, const TegraNVENCOrder *order, uint32_t *fence, const char **why);
uint32_t tegra_nvenc_syncpt(void);  // the job fences' syncpoint
// Sleeps until the fence is reached (syncpoint interrupt), bounded per queued job. Past that bound the jobs in flight
// are cancelled (-5) and the engine is reset for the next one.
int tegra_nvenc_wait(uint32_t fence);
// Once tegra_nvenc_wait(fence) succeeded: 0, or the failure of a job that wrote what the job read.
int tegra_nvenc_input_failure(uint32_t fence);
bool tegra_nvenc_ready(void);

// A stalled job's line (TegraNVENCDump.cpp): the falcon's state, and where the app the job runs is stuck.
const char *tegra_nvenc_falcon_dump(volatile uint8_t *nvenc);
const char *tegra_nvenc_app_dump(volatile uint8_t *nvenc, uint32_t codec);

int tegra_nvenc_publish(void);  // E3
}

// TegraNVENCEngine.cpp: NVENC as an engine of SwitchGpuBacking, its jobs' fences the tokens of the memory they read,
// which other engines and the CPU wait for.
bool tegra_nvenc_engine_start(void);
bool tegra_nvenc_engine_ready(void);
switch_gpu_backing_token tegra_nvenc_token(uint32_t fence);

// TegraNVENCSource.cpp: a checked submit's source surface written into its input buffer by VIC. 0, what the job waits
// for and the route (TEGRA_NVENC_INPUT_*), or a negative errno-style value and the reason. `last`: the connection's last
// job's fence, if any.
bool tegra_nvenc_source_ready(void);
uint32_t tegra_nvenc_source_row_align(void);
int tegra_nvenc_source(const TegraNVENCSubmit *submit, TegraNVENCResolve resolve, void *context, task_t task,
                       const uint32_t *last, TegraNVENCAfter *after, uint32_t *route, const char **why);

#endif
