// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
struct switch_gpu_cached_mapping;

// The caller exclusively owns prepared pages through completion. Mapping ownership can move with that allocation.
int switch_gpu_cached_zero(const uint64_t *pages, uint32_t count, void *cpu,
			   switch_gpu_cached_mapping **mapping, void (*completed)(void *, int),
			   void *context);
void switch_gpu_cached_mapping_drop(switch_gpu_cached_mapping **mapping);
