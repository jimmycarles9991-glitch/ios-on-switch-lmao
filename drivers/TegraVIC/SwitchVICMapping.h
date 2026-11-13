// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchVICMapping;
bool switch_vic_mapping_start();
SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool writable);
SwitchVICMapping *switch_vic_mapping_acquire_range(switch_gpu_backing *backing, bool writable, uint32_t firstPage,
                                                   uint32_t pageCount);
void switch_vic_mapping_release(SwitchVICMapping *mapping);
uint64_t switch_vic_mapping_take_made();
uint64_t switch_vic_mapping_address(const SwitchVICMapping *mapping);

extern "C" uint64_t switch_vic_mapping_create(const uint64_t *pages, uint32_t count, bool writable);
extern "C" void switch_vic_mapping_destroy(uint64_t address, uint32_t count);
