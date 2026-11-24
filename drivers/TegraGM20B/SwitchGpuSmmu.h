// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GPU_SMMU_BIT (1ULL << 34)
#define SWITCH_GPU_SMMU_GRANULE (64ULL << 10)
#define SWITCH_GPU_SMMU_GRANULE_PAGES 16u
#define SWITCH_GPU_SMMU_WINDOW_BASE 0x10000000ULL

static inline uint64_t switch_gpu_smmu_pa(uint64_t iova)
{
	return SWITCH_GPU_SMMU_BIT | (iova & 0xffffffffULL);
}

static inline int switch_gpu_smmu_is_iova(uint64_t pa)
{
	return (pa & SWITCH_GPU_SMMU_BIT) != 0u;
}

/* GM20B selects Tegra SMMU per transaction via physical bit 34; bit 34 = 0 accesses DRAM directly.
 * Compact scattered 64 KiB-multiple allocations and non-alternating 64 KiB chunks whose bit 16
 * collides on a shared 128 KiB comptagline into contiguous 64 KiB SMMU IOVAs. */
static inline int switch_gpu_smmu_should_map(const uint64_t *pas, uint32_t npages)
{
	if (!pas || !npages || (npages % SWITCH_GPU_SMMU_GRANULE_PAGES) != 0u)
		return 0;
	for (uint32_t i = 0; i < npages; i += SWITCH_GPU_SMMU_GRANULE_PAGES) {
		const uint64_t first = pas[i];
		if (first & (SWITCH_GPU_SMMU_GRANULE - 1u))
			return 1;
		for (uint32_t k = 1; k < SWITCH_GPU_SMMU_GRANULE_PAGES; ++k) {
			if (pas[i + k] != first + ((uint64_t)k << 12))
				return 1;
		}
	}
	for (uint32_t i = 0; i + 2u * SWITCH_GPU_SMMU_GRANULE_PAGES <= npages;
	     i += 2u * SWITCH_GPU_SMMU_GRANULE_PAGES) {
		const uint64_t diff = pas[i] ^ pas[i + SWITCH_GPU_SMMU_GRANULE_PAGES];
		if ((diff & SWITCH_GPU_SMMU_GRANULE) == 0u)
			return 1;
	}
	return 0;
}

int switch_gpu_smmu_init(void);
int switch_gpu_smmu_ready(void);
uint64_t switch_gpu_smmu_map(const uint64_t *pas, uint32_t npages);
void switch_gpu_smmu_unmap(uint64_t iova, uint32_t npages);
void switch_gpu_smmu_usage(uint64_t *total, uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif
