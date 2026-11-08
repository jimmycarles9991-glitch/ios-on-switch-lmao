// SPDX-License-Identifier: BSD-2-Clause-Patent
// The TegraNVDEC user client from user space (ABI: drivers/TegraNVDEC/TegraNVDECABI.h). Calls are tunnelled through
// the first selector the process's sandbox admits; buffers are mapped cacheable into this task.
#ifndef NVDEC_CLIENT_H
#define NVDEC_CLIENT_H
#include <stdbool.h>
#include <stdint.h>
#include "../../TegraNVDEC/TegraNVDECABI.h"

typedef struct NVDECClient NVDECClient;

bool nvdec_service_present(void);
NVDECClient *nvdec_open(int *error);  // NULL with the IOReturn when the service is absent or refuses this process
void nvdec_close(NVDECClient *client);
// TegraNVDECInfo.codecs: bit n, codec n decoded on NVDEC.
uint32_t nvdec_codecs(const NVDECClient *client);
// TegraNVDECInfo.features has TEGRA_NVDEC_FEATURE_PRESENT: VIC copies pictures into IOSurfaces (nvdec_present).
bool nvdec_can_present(const NVDECClient *client);
// A decoded picture into an IOSurface of this process (TegraNVDECPresent); 0, or the IOReturn (the picture's range is
// then invalidated for a CPU copy).
int nvdec_present(NVDECClient *client, const TegraNVDECPresent *present);

// The H264Backend buffer and job callbacks (ctx = the client).
int nvdec_alloc(void *client, uint64_t size, uint32_t *handle, void **va);
void nvdec_free(void *client, uint32_t handle);
int nvdec_submit(void *client, const TegraNVDECSubmit *submit, uint32_t *fence);
int nvdec_wait(void *client, const TegraNVDECWait *wait);

#endif
