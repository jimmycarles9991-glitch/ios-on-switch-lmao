// SPDX-License-Identifier: BSD-2-Clause-Patent
// The TegraNVENC user client from user space (ABI: drivers/TegraNVENC/TegraNVENCABI.h). Calls are tunnelled through
// the first selector the process's sandbox admits; buffers are mapped cacheable into this task.
#ifndef NVENC_CLIENT_H
#define NVENC_CLIENT_H
#include <stdbool.h>
#include <stdint.h>
#include "../../TegraNVENC/TegraNVENCABI.h"

typedef struct NVENCClient NVENCClient;

// The service is published; a process asking before NVENC's bring-up has ended waits for it, or for the kernel's word
// that it will not come (TEGRA_NVENC_ABSENT_RESOURCE).
bool nvenc_service_wait(void);
NVENCClient *nvenc_open(int *error);  // NULL with the IOReturn when the service is absent or refuses this process
void nvenc_close(NVENCClient *client);

bool nvenc_info(NVENCClient *client, TegraNVENCInfo *info);  // input layout, chroma reading, memory

// Buffers and jobs (ctx = the client).
int nvenc_alloc(void *client, uint64_t size, uint32_t *handle, void **va);
void nvenc_free(void *client, uint32_t handle);
// 0 with the job's fence and how its input arrived (TEGRA_NVENC_INPUT_*, `input` may be null), or the IOReturn.
int nvenc_submit(void *client, const TegraNVENCSubmit *submit, uint32_t *fence, uint32_t *input);
int nvenc_wait(void *client, const TegraNVENCWait *wait);

#endif
