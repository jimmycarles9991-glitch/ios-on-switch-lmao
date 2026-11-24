// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

struct switch_gpu_backing;
struct tgm_mem;

int peer_allocated_memory_smmu(struct switch_gpu_backing *backing, uint32_t count,
			       struct tgm_mem **out);
int peer_allocated_memory_existing(struct switch_gpu_backing *backing, uint32_t count,
				   struct tgm_mem **out);
int peer_allocated_memory_existing_or_direct(struct switch_gpu_backing *backing, uint32_t count,
					     struct tgm_mem **out);
void (*peer_destroy_address(void))(void *);
