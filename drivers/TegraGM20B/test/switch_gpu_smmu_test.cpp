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
	EXPECT_EQ(static_cast<uint32_t>(TEGRA_SMMU_GPU_ASID), 0xaacu);
	EXPECT_EQ(static_cast<uint32_t>(TEGRA_SMMU_TRANSLATION_ENABLE_4), 0xb98u);
	tegra_smmu_ids_distinct(TEGRA_SMMU_ASID_GPU, TEGRA_SMMU_GPU_ASID);

	const uint64_t pa = switch_gpu_smmu_pa(0x10010000ULL);
	EXPECT_EQ(pa, (1ULL << 34) | 0x10010000ULL);
	EXPECT_TRUE(switch_gpu_smmu_is_iova(pa));
	EXPECT_FALSE(switch_gpu_smmu_is_iova(0x80000000ULL));
	EXPECT_FALSE(switch_gpu_smmu_is_iova(0x17fff0000ULL));
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
}  // namespace
