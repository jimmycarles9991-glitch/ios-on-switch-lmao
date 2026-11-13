// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "TegraNVDEC.h"
#include "../TegraVIC/SwitchVICPicture.h"

struct SwitchNVDECBuffer {
    uint32_t handle;
    TegraNVDECBlock block;
    IOMemoryMap *map;
};

struct SwitchNVDECPark;

bool switch_nvdec_native_output_ready();
int switch_nvdec_output_reusable(const TegraNVDECBlock *block, bool *idle);
// park: where a replaced buffer goes, and where a free one comes back from before anything is allocated.
int switch_nvdec_output_recycle(SwitchNVDECBuffer *buffer, task_t task, SwitchNVDECPark *park = nullptr);
int switch_nvdec_output_picture(const TegraNVDECBlock *block, const TegraNVDECPresent *request,
                                TegraVICPicture *picture);

// Why an export left a picture to VIC's copy, and the sizes of the surface it was for (zero before the lookup).
struct SwitchNVDECExportRefusal {
    const char *why;
    uint64_t allocationBytes, descriptorBytes, baseOffset;
};

// fresh: switch_native_surface_publish's.
int switch_nvdec_output_export(const TegraNVDECBlock *block, const TegraNVDECPresent *request, task_t task,
                               SwitchNVDECExportRefusal *refusal = nullptr, bool *fresh = nullptr);
