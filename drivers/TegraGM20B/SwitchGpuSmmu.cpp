// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include "SwitchGpuSmmu.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUEngine.h"
#include "../TegraSMMU/TegraSMMURamView.h"

namespace
{
// GPUB's clients: gpusrd/gpuswr and gpusrd2/gpuswr2, read-only and set (T210 TRM 18.11.1.68, .153).
constexpr struct {
	uint32_t reg, bits;
} kGpubClients[] = {
	{TEGRA_SMMU_TRANSLATION_ENABLE_2, (1u << 24) | (1u << 25)},
	{TEGRA_SMMU_TRANSLATION_ENABLE_4, (1u << 8) | (1u << 9)},
};

constexpr uint64_t kWindowSizes[] = {1ULL << 30, 512ULL << 20, 256ULL << 20};

TegraSMMUEngine gEngine;  // the compaction window: field 2, its own ASID
TegraSMMURamView gRam;    // RAM at its own address: fields 0 and 1
int gReady;

int attach(char *line, size_t size)
{
	bool initialized = false;
	for (uint64_t windowBytes : kWindowSizes) {
		if (gEngine.init(TEGRA_SMMU_ASID_GPU, SWITCH_GPU_SMMU_WINDOW_BASE, windowBytes,
				 0)) {
			initialized = true;
			break;
		}
	}
	if (!initialized || !gRam.init(TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_ASID_GPU_HIGH)) {
		snprintf(line, size, "no table memory");
		return -12;
	}
	for (const auto &client : kGpubClients) {
		if (gRam.attach(TEGRA_SMMU_GPUB_ASID, client.reg, client.bits, true,
				TEGRA_SMMU_ASID_GPU, gEngine.domain.ptb(), line, size)) {
			tegra_smmu_detach(TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_GPUB_ASID);
			return -5;
		}
	}
	return 0;
}
}  // namespace

// Before any memory object that is not the pool's (gpu_mem.c): bring-up runs on the pool alone.
extern "C" int switch_gpu_smmu_init(void)
{
	if (switch_gpu_smmu_ram_ready())
		return 0;
	char line[160] = {};
	const int ret = attach(line, sizeof(line));
	if (ret) {
		IOLog("tegra-gm20b: GPU DMA not behind the SMMU (%s): every page the GPU is given "
		      "is a raw address\n",
		      line);
		gEngine.release();
		return ret;
	}
	IOLog("tegra-gm20b: GPU DMA through the SMMU (GPUB ASIDs %u, %u and window %u; "
	      "MC %#x = %#x, MC %#x = %#x): the GPU reaches its pool and the pages of its "
	      "memory objects\n",
	      TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_ASID_GPU_HIGH, TEGRA_SMMU_ASID_GPU,
	      TEGRA_SMMU_TRANSLATION_ENABLE_2, tegra_smmu_read(TEGRA_SMMU_TRANSLATION_ENABLE_2),
	      TEGRA_SMMU_TRANSLATION_ENABLE_4, tegra_smmu_read(TEGRA_SMMU_TRANSLATION_ENABLE_4));
	__atomic_store_n(&gReady, 1, __ATOMIC_RELEASE);
	return 0;
}

extern "C" int switch_gpu_smmu_ram_ready(void)
{
	return __atomic_load_n(&gReady, __ATOMIC_ACQUIRE);
}

extern "C" int switch_gpu_smmu_ready(void)
{
	return switch_gpu_smmu_ram_ready();
}

extern "C" uint64_t switch_gpu_smmu_map_reserved(const uint64_t *pas, uint32_t npages,
						 uint64_t reserve_bytes)
{
	if (!switch_gpu_smmu_ready() || !pas || !npages)
		return 0;
	return gEngine.map(pas, npages, true, reserve_bytes);
}

extern "C" uint64_t switch_gpu_smmu_map(const uint64_t *pas, uint32_t npages)
{
	return switch_gpu_smmu_map_reserved(pas, npages, 0);
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

#if DEVELOPMENT || DEBUG
extern "C" void switch_gpu_smmu_stats(char *line, size_t size)
{
	switch_smmu_stats_window(line, size);
}
#endif

extern "C" int switch_gpu_smmu_hold(const uint64_t *pas, uint32_t npages)
{
	if (!npages)
		return 0;
	return switch_gpu_smmu_ram_ready() && gRam.holdPages(pas, npages) ? 0 : -12;
}

extern "C" void switch_gpu_smmu_release_ram(const uint64_t *pages, uint32_t npages)
{
	if (!npages || !switch_gpu_smmu_ram_ready())
		return;
	uint64_t unheld = 0;
	gRam.locked([&](auto &core) {
		for (uint32_t i = 0; i < npages; ++i) {
			const uint64_t pa = pages[i] & ~SWITCH_GPU_SMMU_BIT;
			if (switch_gpu_smmu_is_ram_pa(pages[i]) &&
			    !core.release(pa, TegraSMMUDomain::kPage) && !unheld)
				unheld = pa;
		}
		core.commit();
	});
	if (unheld)
		panic("tegra-gm20b: a release of %u pages found page %#llx not held in GPUB's RAM "
		      "view",
		      npages, static_cast<unsigned long long>(unheld));
}
