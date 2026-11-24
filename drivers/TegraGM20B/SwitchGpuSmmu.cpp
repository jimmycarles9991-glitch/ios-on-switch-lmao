// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include "SwitchGpuSmmu.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUEngine.h"

namespace
{
constexpr TegraSMMUSwgroup kGpuGroups[] = {
	{TEGRA_SMMU_GPU_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2, (1u << 24) | (1u << 25)},
	{TEGRA_SMMU_GPU_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_4, (1u << 8) | (1u << 9)},
};
constexpr uint64_t kWindowSizes[] = {1ULL << 30, 512ULL << 20, 256ULL << 20};

TegraSMMUEngine gEngine;
int gReady;
}  // namespace

extern "C" int switch_gpu_smmu_init(void)
{
	if (__atomic_load_n(&gReady, __ATOMIC_ACQUIRE))
		return 0;
	bool initialized = false;
	for (uint64_t windowBytes : kWindowSizes) {
		if (gEngine.init(TEGRA_SMMU_ASID_GPU, SWITCH_GPU_SMMU_WINDOW_BASE, windowBytes,
				 0)) {
			initialized = true;
			break;
		}
	}
	if (!initialized) {
		IOLog("tegra-gm20b: GPU SMMU init failed: no table memory\n");
		return -12;
	}
	char line[160] = {};
	if (gEngine.attach(kGpuGroups, 2, line, sizeof(line)) != 0) {
		IOLog("tegra-gm20b: GPU SMMU attach failed: %s\n", line);
		gEngine.release();
		return -5;
	}
	__atomic_store_n(&gReady, 1, __ATOMIC_RELEASE);
	return 0;
}

extern "C" int switch_gpu_smmu_ready(void)
{
	return __atomic_load_n(&gReady, __ATOMIC_ACQUIRE);
}

extern "C" uint64_t switch_gpu_smmu_map(const uint64_t *pas, uint32_t npages)
{
	if (!switch_gpu_smmu_ready() || !pas || !npages)
		return 0;
	return gEngine.map(pas, npages, true);
}

extern "C" void switch_gpu_smmu_unmap(uint64_t iova, uint32_t npages)
{
	if (!switch_gpu_smmu_ready() || !iova || !npages)
		return;
	gEngine.unmap(iova, npages);
}

extern "C" void switch_gpu_smmu_usage(uint64_t *total, uint64_t *free_bytes)
{
	if (!switch_gpu_smmu_ready()) {
		if (total)
			*total = 0;
		if (free_bytes)
			*free_bytes = 0;
		return;
	}
	gEngine.usage(total, free_bytes);
}
