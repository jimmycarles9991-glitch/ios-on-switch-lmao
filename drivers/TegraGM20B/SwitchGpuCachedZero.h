// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
struct switch_gpu_cached_mapping;

// The caller exclusively owns prepared pages through completion. Mapping ownership can move with that allocation.
// `cpu`: the pages' cacheable kernel alias, whose lines are invalidated before the fill and after it; null for pages with
// none (a write-combined spare: its VM object's cache-mode change rewrote every alias and cleaned every line), whose
// zero takes no CPU cache operation.
int switch_gpu_cached_zero(const uint64_t *pages, uint32_t count, void *cpu,
			   switch_gpu_cached_mapping **mapping, void (*completed)(void *, int),
			   void *context);
void switch_gpu_cached_mapping_drop(switch_gpu_cached_mapping **mapping);
int switch_gpu_cached_copy(const uint64_t *srcPages, const uint64_t *dstPages, uint32_t count);
