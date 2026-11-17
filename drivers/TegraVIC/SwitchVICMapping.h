// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchVICMapping;
bool switch_vic_mapping_start();
SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool writable);
SwitchVICMapping *switch_vic_mapping_acquire_range(switch_gpu_backing *backing, bool writable, uint32_t firstPage,
                                                   uint32_t pageCount);
// The whole backing, which the caller has just described.
SwitchVICMapping *switch_vic_mapping_acquire_described(switch_gpu_backing *backing, const switch_gpu_backing_info &info,
                                                       bool writable);
void switch_vic_mapping_release(SwitchVICMapping *mapping);
uint64_t switch_vic_mapping_take_made();
// Debug kernels: mappings made since boot, and of them remakes of one evicted for room; zeros on release kernels.
void switch_vic_mapping_totals(uint64_t *made, uint64_t *remade);
void switch_vic_totals_register();
uint64_t switch_vic_mapping_address(const SwitchVICMapping *mapping);

extern "C" uint64_t switch_vic_mapping_create(const uint64_t *pages, uint32_t count, bool writable);
extern "C" void switch_vic_mapping_destroy(uint64_t address, uint32_t count);
