// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

using IOOptionBits = uint64_t;

class IOBufferMemoryDescriptor {};

void switch_test_dma_cache(const char *operation, uintptr_t address);
void switch_test_dma_barrier();
bool switch_dma_pa_managed(uint64_t address);
#define SWITCH_DCACHE_OP(operation, address) switch_test_dma_cache(operation, address)
#define SWITCH_DCACHE_BARRIER()              switch_test_dma_barrier()
