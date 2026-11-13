// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <array>
#include <random>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuPoolAllocation.h"
#include "drivers/TegraGM20B/TegraGM20BPoolPolicy.h"

namespace
{
constexpr unsigned Bits = sizeof(unsigned long) * CHAR_BIT;

void bit(std::vector<unsigned long> &words, unsigned at, bool value)
{
	if (value)
		words[at / Bits] |= 1UL << (at % Bits);
	else
		words[at / Bits] &= ~(1UL << (at % Bits));
}

bool bit(const std::vector<unsigned long> &words, unsigned at)
{
	return (words[at / Bits] >> (at % Bits)) & 1;
}

long oracle(const std::vector<unsigned long> &used, const std::vector<unsigned long> &dirty,
	    unsigned pages, unsigned count, unsigned align, bool clean)
{
	for (unsigned start = 0; start + count <= pages; start += align) {
		bool fits = true;
		for (unsigned i = start; i < start + count; ++i)
			fits &= !bit(used, i) && (!clean || !bit(dirty, i));
		if (fits)
			return start;
	}
	return -1;
}

TEST(PoolAllocation, BitmapRangeFinderMatchesIndependentOracle)
{
	std::mt19937 random(81);
	for (unsigned iteration = 0; iteration < 30000; ++iteration) {
		const unsigned pages = 1 + random() % 257;
		std::vector<unsigned long> used((pages + Bits - 1) / Bits), dirty(used.size());
		for (unsigned i = 0; i < pages; ++i) {
			bit(used, i, random() % 13 == 0);
			bit(dirty, i, random() % 11 == 0);
		}
		const unsigned count = 1 + random() % pages, align = 1u << (random() % 7);
		for (bool clean : {false, true})
			ASSERT_EQ(switch_gpu_pool_find(used.data(), dirty.data(), pages, count,
						       align, clean),
				  oracle(used, dirty, pages, count, align, clean));
	}
}

TEST(PoolAllocation, WarmAllocationReusesBoundedExtentMetadata)
{
	switch_gpu_pool_extents cache{};
	switch_gpu_pool_remember(&cache, 0, 65536, 1);
	for (unsigned frame = 0; frame < 10000; ++frame) {
		const long start = switch_gpu_pool_cached(&cache, 128, 32, 1);
		ASSERT_GE(start, 0);
		switch_gpu_pool_claim(&cache, start, 128);
		switch_gpu_pool_remember(&cache, start, 128, 0);
		EXPECT_NE(switch_gpu_pool_cached(&cache, 128, 32, 1), start);
		switch_gpu_pool_claim(&cache, start, 128);
		switch_gpu_pool_remember(&cache, start, 128, 1);
	}
}

TEST(PoolAllocation, EveryExternalClaimInvalidatesOverlappingCachedRanges)
{
	switch_gpu_pool_extents cache{};
	switch_gpu_pool_remember(&cache, 0, 1024, 1);
	switch_gpu_pool_claim(&cache, 64, 640);
	EXPECT_EQ(switch_gpu_pool_cached(&cache, 64, 1, 1), 0);
	switch_gpu_pool_claim(&cache, 0, 64);
	EXPECT_EQ(switch_gpu_pool_cached(&cache, 128, 64, 1), 704);
	EXPECT_EQ(switch_gpu_pool_cached(&cache, 321, 1, 1), -1);
}

TEST(PoolAllocation, FragmentedMetadataNeverClaimsAllocatedOrDirtyPages)
{
	constexpr unsigned pages = 4096;
	std::vector<unsigned long> used(pages / Bits), dirty(pages / Bits);
	switch_gpu_pool_extents cache{};
	switch_gpu_pool_remember(&cache, 0, pages, 1);
	std::vector<std::pair<unsigned, unsigned>> live;
	std::mt19937 random(93);
	for (unsigned iteration = 0; iteration < 100000; ++iteration) {
		if (!live.empty() && random() % 3 == 0) {
			const unsigned index = random() % live.size();
			const auto [start, count] = live[index];
			const bool clean = random() % 2;
			for (unsigned i = start; i < start + count; ++i) {
				bit(used, i, false);
				bit(dirty, i, !clean);
			}
			switch_gpu_pool_remember(&cache, start, count, clean);
			live.erase(live.begin() + index);
		} else {
			const unsigned count = 1 + random() % 128, align = 1u << (random() % 6);
			const bool clean = random() % 2;
			long start = switch_gpu_pool_cached(&cache, count, align, clean);
			if (start < 0)
				start = switch_gpu_pool_find(used.data(), dirty.data(), pages,
							     count, align, clean);
			if (start < 0)
				continue;
			for (unsigned i = start; i < unsigned(start) + count; ++i) {
				ASSERT_FALSE(bit(used, i));
				if (clean)
					ASSERT_FALSE(bit(dirty, i));
				bit(used, i, true);
			}
			switch_gpu_pool_claim(&cache, start, count);
			live.emplace_back(start, count);
		}
	}
}

TEST(PoolAllocation, CleanCounterHandlesPartialWordsAndMixedChunks)
{
	std::vector<unsigned long> dirty(5, 0);
	for (unsigned i = 0; i < 320; ++i)
		bit(dirty, i, i % 3 == 0);
	for (unsigned start = 0; start < 150; ++start)
		for (unsigned count = 1; count < 150; ++count) {
			unsigned expected = 0;
			for (unsigned i = start; i < start + count; ++i)
				expected += !bit(dirty, i);
			ASSERT_EQ(switch_gpu_pool_clean_count(dirty.data(), start, count),
				  expected);
		}
}

TEST(PoolAllocation, ControlReserveIsExplicitAndBoundedPerConnection)
{
	using namespace tgm_pool;
	EXPECT_FALSE(controlReserve(false, 0, 4096));
	EXPECT_TRUE(controlReserve(true, ControlClientBytes - 4096, 4096));
	EXPECT_FALSE(controlReserve(true, ControlClientBytes, 4096));
	EXPECT_FALSE(controlReserve(true, UINT64_MAX, 4096));
	EXPECT_LT(KernelReadyBytes, ControlReadyBytes);
}
}
