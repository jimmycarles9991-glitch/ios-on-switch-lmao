/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "SwitchGpuBacking.h"
#include "SwitchGpuCachedBuffer.h"
#include "SwitchGpuCachedZero.h"
switch_gpu_backing *switch_gpu_pin_memory(const uint64_t *pages, uint32_t count,
					  uint32_t surface_id, IOMemoryDescriptor *memory,
					  bool prepare);
switch_gpu_backing *switch_gpu_pin_chunks(const uint64_t *pages, uint32_t count);
// The backing takes its own prepare and reference of buffer.memory, and buffer.map with its mapping: the caller drops
// neither once this succeeds.
switch_gpu_backing *switch_gpu_pin_cached(const uint64_t *pages, uint32_t count,
					  uint32_t visible_count,
					  const SwitchGpuCachedBuffer &buffer,
					  switch_gpu_cached_mapping *mapping = nullptr);
