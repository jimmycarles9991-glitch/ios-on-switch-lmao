// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchNVJPGMapping {
    uint64_t address;
    const uint64_t *pages;
    uint32_t count, mapped, available;
    SwitchNVJPGMapping *next;
};

void switch_nvjpg_mapping_start(uint64_t sink);
SwitchNVJPGMapping *switch_nvjpg_mapping(switch_gpu_backing *backing, uint32_t pages, int pid = 0);
