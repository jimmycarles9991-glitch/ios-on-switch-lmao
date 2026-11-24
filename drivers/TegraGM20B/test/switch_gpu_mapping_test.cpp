// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../SwitchGpuMapping.h"

namespace
{
constexpr uint64_t kPage = 4096, kBig = 64 * 1024, kVa = 0x40000000;

std::vector<uint64_t> pages(unsigned count, uint64_t base = 0x80000000)
{
	std::vector<uint64_t> result(count);
	for (unsigned i = 0; i < count; ++i)
		result[i] = base + i * kPage;
	return result;
}

std::vector<switch_gpu_mapping_run> plan(const std::vector<uint64_t> &physical, uint64_t va,
					 uint64_t offset, uint64_t bytes)
{
	std::vector<switch_gpu_mapping_run> result;
	while (bytes) {
		switch_gpu_mapping_run run{};
		if (!switch_gpu_mapping_next(physical.data(),
					     static_cast<uint32_t>(physical.size()), va, offset,
					     bytes, &run) ||
		    !run.bytes || run.bytes > bytes) {
			ADD_FAILURE() << "invalid mapping run";
			return {};
		}
		result.push_back(run);
		va += run.bytes;
		offset += run.bytes;
		bytes -= run.bytes;
	}
	return result;
}

TEST(GpuMapping, AlignedContiguousExtentsCoalesce)
{
	const auto runs = plan(pages(48), kVa, 0, 3 * kBig);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 16);
	EXPECT_EQ(runs[0].bytes, 3 * kBig);
}

TEST(GpuMapping, TailUsesOnlyOwnedSmallPages)
{
	const auto runs = plan(pages(19), kVa, 0, 19 * kPage);
	ASSERT_EQ(runs.size(), 2u);
	EXPECT_EQ(runs[0].shift, 16);
	EXPECT_EQ(runs[0].bytes, kBig);
	EXPECT_EQ(runs[1].shift, 12);
	EXPECT_EQ(runs[1].bytes, 3 * kPage);
}

TEST(GpuMapping, PartialBindKeepsSmallHeadAndTail)
{
	const auto runs = plan(pages(48), kVa + kPage, kPage, 33 * kPage);
	ASSERT_EQ(runs.size(), 3u);
	EXPECT_EQ(runs[0].shift, 12);
	EXPECT_EQ(runs[0].bytes, 15 * kPage);
	EXPECT_EQ(runs[1].shift, 16);
	EXPECT_EQ(runs[1].bytes, kBig);
	EXPECT_EQ(runs[2].shift, 12);
	EXPECT_EQ(runs[2].bytes, 2 * kPage);
}

TEST(GpuMapping, VirtualAndMemoryPhaseMismatchStaysSmall)
{
	const auto runs = plan(pages(48), kVa, kPage, 32 * kPage);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 12);
	EXPECT_EQ(runs[0].bytes, 32 * kPage);
}

TEST(GpuMapping, PhysicalMisalignmentStaysSmall)
{
	const auto runs = plan(pages(32, 0x80001000), kVa, 0, 32 * kPage);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 12);
}

TEST(GpuMapping, OneFragmentedChunkDoesNotDemoteTheWholeAllocation)
{
	auto physical = pages(48);
	physical[20] += 8 * kBig;
	const auto runs = plan(physical, kVa, 0, physical.size() * kPage);
	ASSERT_EQ(runs.size(), 3u);
	EXPECT_EQ(runs[0].shift, 16);
	EXPECT_EQ(runs[1].shift, 12);
	EXPECT_EQ(runs[2].shift, 16);
	for (const auto &run : runs)
		EXPECT_EQ(run.bytes, kBig);
}

TEST(GpuMapping, IndependentAlignedPhysicalChunksCanShareOneRun)
{
	auto physical = pages(32);
	for (unsigned i = 16; i < physical.size(); ++i)
		physical[i] += 16 * kBig;
	const auto runs = plan(physical, kVa, 0, physical.size() * kPage);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 16);
	EXPECT_EQ(runs[0].bytes, 2 * kBig);
}

TEST(GpuMapping, TruncatedPageVectorCannotFormABigPage)
{
	const auto physical = pages(15);
	EXPECT_FALSE(switch_gpu_mapping_big_page(physical.data(), 15, 0));
	EXPECT_FALSE(switch_gpu_mapping_big_page(physical.data(), 15, 15));
	EXPECT_FALSE(switch_gpu_mapping_big_page(physical.data(), 15, UINT32_MAX));
	const auto runs = plan(physical, kVa, 0, physical.size() * kPage);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 12);
}

TEST(GpuMapping, InvalidAndOverflowingExtentsAreRejected)
{
	const auto physical = pages(16);
	switch_gpu_mapping_run run{};
	for (const auto extent :
	     std::array<std::array<uint64_t, 3>, 7>{{{kVa, 0, 0},
						     {kVa + 1, 0, kPage},
						     {kVa, 1, kPage},
						     {kVa, 0, kPage + 1},
						     {kVa, kBig, kPage},
						     {kVa, 0, kBig + kPage},
						     {UINT64_MAX - kPage + 1, 0, kPage}}})
		EXPECT_FALSE(switch_gpu_mapping_next(physical.data(), 16, extent[0], extent[1],
						     extent[2], &run));
	EXPECT_FALSE(switch_gpu_mapping_next(nullptr, 16, kVa, 0, kPage, &run));
	EXPECT_FALSE(switch_gpu_mapping_next(physical.data(), 16, kVa, 0, kPage, nullptr));
}

TEST(GpuMapping, EverySubrangeCoversExactlyItsOwnedPages)
{
	for (unsigned pattern = 0; pattern < 3; ++pattern) {
		auto physical = pages(64);
		if (pattern == 1)
			physical[21] += 16 * kBig;
		if (pattern == 2)
			for (unsigned i = 32; i < 48; ++i)
				physical[i] += 32 * kBig;
		for (unsigned start = 0; start < physical.size(); ++start) {
			for (unsigned count = 1; count <= physical.size() - start; ++count) {
				for (unsigned phase : {0u, 1u, 9u, 15u}) {
					uint64_t va = kVa + phase * kPage;
					uint64_t offset = start * kPage;
					const auto runs = plan(physical, va, offset, count * kPage);
					ASSERT_FALSE(runs.empty());
					uint64_t total = 0;
					for (const auto &run : runs) {
						const uint64_t unit = uint64_t{1} << run.shift;
						ASSERT_TRUE(run.shift == 12 || run.shift == 16);
						ASSERT_EQ((va | offset | run.bytes) & (unit - 1),
							  0u);
						ASSERT_LE(run.bytes, (count * kPage) - total);
						if (run.shift == 16) {
							for (uint64_t at = 0; at < run.bytes;
							     at += kBig) {
								const size_t first =
									(offset + at) / kPage;
								ASSERT_LE(first + 16,
									  physical.size());
								EXPECT_EQ(physical[first] &
										  (kBig - 1),
									  0u);
								for (unsigned page = 0; page < 16;
								     ++page)
									EXPECT_EQ(
										physical[first +
											 page],
										physical[first] +
											page * kPage);
							}
						}
						total += run.bytes;
						va += run.bytes;
						offset += run.bytes;
					}
					ASSERT_EQ(total, count * kPage);
				}
			}
		}
	}
}
}  // namespace

TEST(GpuMapping, AlignedPhysicalInteriorCanStartAnIndependentBigView)
{
	const auto physical = pages(32, 0x80008000);
	const auto runs = plan(physical, kVa, 8 * kPage, 24 * kPage);
	ASSERT_EQ(runs.size(), 2u);
	EXPECT_EQ(runs[0].shift, 16);
	EXPECT_EQ(runs[0].bytes, kBig);
	EXPECT_EQ(runs[1].shift, 12);
	EXPECT_EQ(runs[1].bytes, 8 * kPage);
}

TEST(GpuMapping, BigBytesCountsWhat64KiBPtesCouldMap)
{
	constexpr uint64_t kSurface = 3686400, kBig = 64 * 1024;
	const uint32_t count = uint32_t((kSurface + 4095) / 4096);
	std::vector<uint64_t> pages(count);
	for (uint32_t i = 0; i < count; ++i)
		pages[i] = 0x80000000ull + uint64_t(i) * 4096;
	EXPECT_EQ(switch_gpu_mapping_big_bytes(pages.data(), count, 0x40000000, 0, kSurface),
		  56 * kBig);
	EXPECT_EQ(switch_gpu_mapping_big_bytes(pages.data(), count, 0x40001000, 0, kSurface - 4096),
		  0u);
	for (uint32_t i = 0; i < count; i += 2)
		pages[i] += 0x100000000ull;
	EXPECT_EQ(switch_gpu_mapping_big_bytes(pages.data(), count, 0x40000000, 0, kSurface), 0u);
}
