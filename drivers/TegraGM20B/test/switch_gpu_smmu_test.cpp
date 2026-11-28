// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../SwitchGpuSmmu.h"
#include "../../TegraSMMU/TegraSMMU.h"

namespace
{
std::vector<uint64_t> contiguous(uint64_t base, uint32_t pages)
{
	std::vector<uint64_t> pas(pages);
	for (uint32_t i = 0; i < pages; ++i)
		pas[i] = base + (uint64_t(i) << 12);
	return pas;
}

TEST(SwitchGpuSmmu, RegistersAndBit34EncodingMatchTegra210)
{
	EXPECT_EQ(static_cast<uint32_t>(TEGRA_SMMU_ASID_GPU), 117u);
	EXPECT_EQ(static_cast<uint32_t>(TEGRA_SMMU_GPUB_ASID), 0xaacu);
	EXPECT_EQ(static_cast<uint32_t>(TEGRA_SMMU_TRANSLATION_ENABLE_4), 0xb98u);
	tegra_smmu_ids_distinct(TEGRA_SMMU_ASID_GPU, TEGRA_SMMU_GPUB_ASID);

	// a[34] = 1 selects GPUB; va[33:32] = 2 its window field, 0 and 1 RAM at its own address.
	const uint64_t pa = switch_gpu_smmu_pa(0x10010000ULL);
	EXPECT_EQ(pa, (1ULL << 34) | (2ULL << 32) | 0x10010000ULL);
	EXPECT_TRUE(switch_gpu_smmu_is_iova(pa));
	EXPECT_FALSE(switch_gpu_smmu_is_ram_pa(pa));
	EXPECT_FALSE(switch_gpu_smmu_is_iova(0x80000000ULL));
	EXPECT_FALSE(switch_gpu_smmu_is_iova(0x17fff0000ULL));
	EXPECT_EQ(switch_gpu_smmu_ram_pa(0x17fff0000ULL), (1ULL << 34) | 0x17fff0000ULL);
	EXPECT_TRUE(switch_gpu_smmu_is_ram_pa(switch_gpu_smmu_ram_pa(0x80000000ULL)));
	EXPECT_TRUE(switch_gpu_smmu_is_ram_pa(switch_gpu_smmu_ram_pa(0x17fff0000ULL)));
	EXPECT_FALSE(switch_gpu_smmu_is_iova(switch_gpu_smmu_ram_pa(0x17fff0000ULL)));
	EXPECT_FALSE(switch_gpu_smmu_is_ram_pa(0x17fff0000ULL))
		<< "a raw address is the GPU swgroup's, untranslated";
}

TEST(SwitchGpuSmmu, SmallAndAlreadyAlignedContiguousRangesBypassSmmu)
{
	const auto small = contiguous(0x80001000ULL, 15);
	EXPECT_FALSE(switch_gpu_smmu_should_map(small.data(), uint32_t(small.size())));
	EXPECT_FALSE(switch_gpu_smmu_should_map(nullptr, 32));

	const auto aligned64 = contiguous(0x80000000ULL, 16);
	EXPECT_FALSE(switch_gpu_smmu_should_map(aligned64.data(), uint32_t(aligned64.size())));

	const auto aligned128 = contiguous(0x100000000ULL, 32);
	EXPECT_FALSE(switch_gpu_smmu_should_map(aligned128.data(), uint32_t(aligned128.size())));

	const auto alignedWithTail = contiguous(0x80000000ULL, 35);
	EXPECT_FALSE(switch_gpu_smmu_should_map(alignedWithTail.data(),
						uint32_t(alignedWithTail.size())));

	auto scatteredTail = contiguous(0x80000000ULL, 17);
	scatteredTail[3] += 0x4000;
	EXPECT_FALSE(
		switch_gpu_smmu_should_map(scatteredTail.data(), uint32_t(scatteredTail.size())));

	auto scatteredPanel = contiguous(0x80001000ULL, 900);
	scatteredPanel[10] += 0x8000;
	EXPECT_FALSE(
		switch_gpu_smmu_should_map(scatteredPanel.data(), uint32_t(scatteredPanel.size())));
}

TEST(SwitchGpuSmmu, ScatteredMisalignedAndCollidingParityChunksUseSmmu)
{
	auto scattered = contiguous(0x80000000ULL, 16);
	scattered[8] += 0x4000;
	EXPECT_TRUE(switch_gpu_smmu_should_map(scattered.data(), uint32_t(scattered.size())));

	const auto misaligned = contiguous(0x80004000ULL, 16);
	EXPECT_TRUE(switch_gpu_smmu_should_map(misaligned.data(), uint32_t(misaligned.size())));

	auto sameParity = contiguous(0x80000000ULL, 16);
	auto secondEven = contiguous(0x80040000ULL, 16);
	sameParity.insert(sameParity.end(), secondEven.begin(), secondEven.end());
	EXPECT_TRUE(switch_gpu_smmu_should_map(sameParity.data(), uint32_t(sameParity.size())));

	auto alternating = contiguous(0x80000000ULL, 16);
	auto secondOdd = contiguous(0x80050000ULL, 16);
	alternating.insert(alternating.end(), secondOdd.begin(), secondOdd.end());
	EXPECT_FALSE(switch_gpu_smmu_should_map(alternating.data(), uint32_t(alternating.size())));
}

TEST(SwitchGpuSmmu, BigPagePrefixCoversNon64KMultiplesAndSkipsAlreadyAlignedRuns)
{
	EXPECT_EQ(switch_gpu_smmu_prefix_pages(0), 0u);
	EXPECT_EQ(switch_gpu_smmu_prefix_pages(15), 0u);
	EXPECT_EQ(switch_gpu_smmu_prefix_pages(16), 16u);
	EXPECT_EQ(switch_gpu_smmu_prefix_pages(17), 16u);
	EXPECT_EQ(switch_gpu_smmu_prefix_pages(90), 80u);
	EXPECT_EQ(switch_gpu_smmu_prefix_pages(135), 128u);
	EXPECT_EQ(switch_gpu_smmu_prefix_pages(900), 896u);

	const auto small = contiguous(0x80001000ULL, 15);
	EXPECT_FALSE(switch_gpu_smmu_should_map_big(small.data(), uint32_t(small.size())));
	EXPECT_FALSE(switch_gpu_smmu_should_map_big(nullptr, 900));

	// Uncompressed big pages need no bit-16 parity alternation: two aligned even blocks bypass SMMU.
	auto sameParityAligned = contiguous(0x80000000ULL, 16);
	auto secondEven = contiguous(0x80040000ULL, 16);
	sameParityAligned.insert(sameParityAligned.end(), secondEven.begin(), secondEven.end());
	EXPECT_FALSE(switch_gpu_smmu_should_map_big(sameParityAligned.data(),
						    uint32_t(sameParityAligned.size())));

	const auto alignedPanel = contiguous(0x80000000ULL, 900);
	EXPECT_FALSE(
		switch_gpu_smmu_should_map_big(alignedPanel.data(), uint32_t(alignedPanel.size())));

	for (uint32_t npages : {16u, 17u, 90u, 135u, 900u}) {
		auto pages = contiguous(0x80001000ULL, npages);
		pages[7] += 0x4000;
		EXPECT_TRUE(switch_gpu_smmu_should_map_big(pages.data(), npages)) << npages;
	}

	// Tail page aliasing a prefix page must bypass SMMU so L2 never sees BIT(34)|iova and direct PA.
	auto tailAlias = contiguous(0x80001000ULL, 900);
	tailAlias[898] = tailAlias[42];
	EXPECT_FALSE(switch_gpu_smmu_should_map_big(tailAlias.data(), uint32_t(tailAlias.size())));
}

TEST(SwitchGpuSmmu, PageUniquenessRejectsDuplicatesInvalidPasAndOversizedAllocations)
{
	EXPECT_EQ(switch_gpu_smmu_pfn_table_cap(0), 0u);
	EXPECT_EQ(switch_gpu_smmu_pfn_table_cap(16), 64u);
	EXPECT_EQ(switch_gpu_smmu_pfn_table_cap(32), 64u);
	EXPECT_EQ(switch_gpu_smmu_pfn_table_cap(33), 128u);
	EXPECT_EQ(switch_gpu_smmu_pfn_table_cap(900), 2048u);
	EXPECT_EQ(switch_gpu_smmu_pfn_table_cap((1u << 18) + 1u), 0u);

	auto uniqueCheck = [](const std::vector<uint64_t> &pas) {
		const uint32_t cap = switch_gpu_smmu_pfn_table_cap(uint32_t(pas.size()));
		if (!cap)
			return 0;
		std::vector<uint32_t> table(cap, 0u);
		return switch_gpu_smmu_pages_unique(pas.data(), uint32_t(pas.size()), table.data(),
						    cap);
	};

	const auto panel = contiguous(0x80001000ULL, 900);
	EXPECT_TRUE(uniqueCheck(panel));

	auto dupInPrefix = panel;
	dupInPrefix[200] = dupInPrefix[3];
	EXPECT_FALSE(uniqueCheck(dupInPrefix));

	auto dupAcrossTail = panel;
	dupAcrossTail[899] = dupAcrossTail[100];
	EXPECT_FALSE(uniqueCheck(dupAcrossTail));

	auto dupInTail = panel;
	dupInTail[899] = dupInTail[896];
	EXPECT_FALSE(uniqueCheck(dupInTail));

	auto badZero = contiguous(0x80000000ULL, 16);
	badZero[4] = 0;
	EXPECT_FALSE(uniqueCheck(badZero));

	auto badUnaligned = contiguous(0x80000000ULL, 16);
	badUnaligned[4] |= 0x100;
	EXPECT_FALSE(uniqueCheck(badUnaligned));

	auto badHigh = contiguous(0x80000000ULL, 16);
	badHigh[4] = SWITCH_GPU_SMMU_BIT;
	EXPECT_FALSE(uniqueCheck(badHigh));
}
}  // namespace
