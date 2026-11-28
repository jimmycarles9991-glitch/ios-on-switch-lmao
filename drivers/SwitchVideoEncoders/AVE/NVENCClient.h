// SPDX-License-Identifier: BSD-2-Clause-Patent
// The TegraNVENC user client from user space (ABI: drivers/TegraNVENC/TegraNVENCABI.h). Calls are tunnelled through
// the first selector the process's sandbox admits; buffers are mapped cacheable into this task.
#ifndef NVENC_CLIENT_H
#define NVENC_CLIENT_H
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include "../../TegraNVENC/TegraNVENCABI.h"
#include "../../TegraNVENC/nvidia/clc5b7.h"

typedef struct NVENCClient NVENCClient;

enum {
    NVENC_STATUS_BYTES = 128,
};

typedef void (*NVENCDone)(void *context, uint32_t fence, int result, uint32_t error_status, uint32_t ucode_error_status,
                          uint32_t total_bit_count);

// The service is published; a process asking before NVENC's bring-up has ended waits for it, or for the kernel's word
// that it will not come (TEGRA_NVENC_ABSENT_RESOURCE).
bool nvenc_service_wait(void);
NVENCClient *nvenc_open(int *error);  // NULL with the IOReturn when the service is absent or refuses this process
void nvenc_close(NVENCClient *client);

bool nvenc_info(NVENCClient *client, TegraNVENCInfo *info);  // input layout, chroma reading, memory

// Buffers and jobs (ctx = the client).
int nvenc_alloc(void *client, uint64_t size, uint32_t *handle, void **va);
void nvenc_free(void *client, uint32_t handle);
void *nvenc_buffer_va(const NVENCClient *client, uint32_t handle);
TegraNVENCSubmit *nvenc_submit_buffer(NVENCClient *client);
TegraNVENCSubmit *nvenc_tile_submit_buffer(NVENCClient *client, uint32_t slot);
// 0 with the job's fence and how its input arrived (TEGRA_NVENC_INPUT_*, `input` may be null), or the IOReturn.
int nvenc_submit(void *client, const TegraNVENCSubmit *submit, uint32_t *fence, uint32_t *input);
int nvenc_wait(void *client, const TegraNVENCWait *wait);

bool nvenc_listen(NVENCClient *client, dispatch_queue_t queue, NVENCDone done, void *context);
void nvenc_stop_listening(NVENCClient *client);
bool nvenc_completions_on_port(const NVENCClient *client);

static inline bool nvenc_status_range(const TegraNVENCSubmit *submit, TegraNVENCRange *range)
{
    for (uint32_t i = 0; i < submit->entry_count && i < TEGRA_NVENC_MAX_ENTRIES; i++) {
        const TegraNVENCEntry *e = &submit->entries[i];
        if (e->method == NVC5B7_SET_OUT_ENC_STATUS && e->kind == TEGRA_NVENC_ENTRY_RELOC) {
            *range = (TegraNVENCRange){e->value, e->offset, NVENC_STATUS_BYTES, 0};
            return true;
        }
    }
    return false;
}

#endif
