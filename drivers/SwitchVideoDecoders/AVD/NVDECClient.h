// SPDX-License-Identifier: BSD-2-Clause-Patent
// The TegraNVDEC user client from user space (ABI: drivers/TegraNVDEC/TegraNVDECABI.h). Calls are tunnelled through
// the first selector the process's sandbox admits; buffers are mapped cacheable into this task.
#ifndef NVDEC_CLIENT_H
#define NVDEC_CLIENT_H
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include "../../TegraNVDEC/SwitchNVDECProbeABI.h"
#include "../../TegraNVDEC/TegraNVDECABI.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"

#define NVDEC_STATUS_BYTES 56  // sizeof(nvdec_status_s)

typedef struct NVDECClient NVDECClient;
// A job's completion: result 0, or the kernel's failure for it; then what NVDEC's status record says.
typedef void (*NVDECDone)(void *context, uint32_t fence, int result, uint32_t error_status, uint32_t mbs_correct);

bool nvdec_service_present(void);
// The codecs the service says it decodes (TEGRA_NVDEC_DECODES_*), found without opening it: TegraNVDECInfo.codecs'
// bits for H.264, VP9, HEVC, MPEG-4 Part 2 and MPEG-1/2.
uint32_t nvdec_service_codecs(void);
NVDECClient *nvdec_open(int *error);  // NULL with the IOReturn when the service is absent or refuses this process
void nvdec_close(NVDECClient *client);
// TegraNVDECInfo.codecs: bit n, codec n decoded on NVDEC.
uint32_t nvdec_codecs(const NVDECClient *client);
// TegraNVDECInfo.jobs_in_flight: a decoder cycling one more input buffer than this never waits to write one.
uint32_t nvdec_jobs_in_flight(const NVDECClient *client);
// The buffer's address in this task, NULL for a handle the client does not hold.
void *nvdec_buffer_va(const NVDECClient *client, uint32_t handle);

// The submit's status record (its SET_NVDEC_STATUS_OFFSET relocation).
static inline bool nvdec_status_range(const TegraNVDECSubmit *submit, TegraNVDECRange *range)
{
    for (uint32_t i = 0; i < submit->entry_count && i < TEGRA_NVDEC_MAX_ENTRIES; i++) {
        const TegraNVDECEntry *e = &submit->entries[i];
        if (e->method == NVC5B0_SET_NVDEC_STATUS_OFFSET && e->kind == TEGRA_NVDEC_ENTRY_RELOC) {
            *range = (TegraNVDECRange){e->value, e->offset, NVDEC_STATUS_BYTES, 0};
            return true;
        }
    }
    return false;
}

// From now on each job this client submits reports to `done` on the serial `queue` (NVDECListener.h); false where the
// kernel has no completions (an older ABI) or the client listens already.
bool nvdec_listen(NVDECClient *client, dispatch_queue_t queue, NVDECDone done, void *context);
// No `done` starts once this returns; one running on the queue finishes (the caller may be on it). nvdec_close stops.
void nvdec_stop_listening(NVDECClient *client);
// The completions come as TegraNVDEC's port messages, not from the refused port's Wait route.
bool nvdec_completions_on_port(const NVDECClient *client);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The connection's buffer counts (a debug kernel's SwitchNVDECMethodBufferStats); false from a release kernel.
bool nvdec_buffer_stats(const NVDECClient *client, SwitchNVDECBufferStats *stats);
#endif
// TegraNVDECInfo.features has TEGRA_NVDEC_FEATURE_PRESENT: VIC copies pictures into IOSurfaces (nvdec_present).
bool nvdec_can_present(const NVDECClient *client);
bool switch_nvdec_can_export(const NVDECClient *client);
int switch_nvdec_export(NVDECClient *client, const TegraNVDECPresent *picture);
int switch_nvdec_recycle(void *client, uint32_t handle, uint64_t bytes, void **address);
// A decoded picture into an IOSurface of this process (TegraNVDECPresent); 0, or the IOReturn (the picture's range is
// then invalidated for a CPU copy).
int nvdec_present(NVDECClient *client, const TegraNVDECPresent *present);

// The H264Backend buffer and job callbacks (ctx = the client).
int nvdec_alloc(void *client, uint64_t size, uint32_t *handle, void **va);
void nvdec_free(void *client, uint32_t handle);
int nvdec_submit(void *client, const TegraNVDECSubmit *submit, TegraNVDECFence *reply);
int nvdec_wait(void *client, const TegraNVDECWait *wait);

#endif
