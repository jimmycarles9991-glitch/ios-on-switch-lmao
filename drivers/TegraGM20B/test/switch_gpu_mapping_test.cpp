// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../SwitchGpuMapping.h"

namespace
{
constexpr uint64_t kPage = 4096, kBig = 128 * 1024, kVa = 0x40000000;

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
	const auto runs = plan(pages(96), kVa, 0, 3 * kBig);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 17);
	EXPECT_EQ(runs[0].bytes, 3 * kBig);
}

TEST(GpuMapping, TailUsesOnlyOwnedSmallPages)
{
	const auto runs = plan(pages(35), kVa, 0, 35 * kPage);
	ASSERT_EQ(runs.size(), 2u);
	EXPECT_EQ(runs[0].shift, 17);
	EXPECT_EQ(runs[0].bytes, kBig);
	EXPECT_EQ(runs[1].shift, 12);
	EXPECT_EQ(runs[1].bytes, 3 * kPage);
}

TEST(GpuMapping, PartialBindKeepsSmallHeadAndTail)
{
	const auto runs = plan(pages(96), kVa + kPage, kPage, 65 * kPage);
	ASSERT_EQ(runs.size(), 3u);
	EXPECT_EQ(runs[0].shift, 12);
	EXPECT_EQ(runs[0].bytes, 31 * kPage);
	EXPECT_EQ(runs[1].shift, 17);
	EXPECT_EQ(runs[1].bytes, kBig);
	EXPECT_EQ(runs[2].shift, 12);
	EXPECT_EQ(runs[2].bytes, 2 * kPage);
}

TEST(GpuMapping, VirtualAndMemoryPhaseMismatchStaysSmall)
{
	const auto runs = plan(pages(96), kVa, kPage, 64 * kPage);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 12);
	EXPECT_EQ(runs[0].bytes, 64 * kPage);
}

TEST(GpuMapping, PhysicalMisalignmentStaysSmall)
{
	const auto runs = plan(pages(64, 0x80001000), kVa, 0, 64 * kPage);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 12);
}

TEST(GpuMapping, OneFragmentedChunkDoesNotDemoteTheWholeAllocation)
{
	auto physical = pages(96);
	physical[40] += 8 * kBig;
	const auto runs = plan(physical, kVa, 0, physical.size() * kPage);
	ASSERT_EQ(runs.size(), 3u);
	EXPECT_EQ(runs[0].shift, 17);
	EXPECT_EQ(runs[1].shift, 12);
	EXPECT_EQ(runs[2].shift, 17);
	for (const auto &run : runs)
		EXPECT_EQ(run.bytes, kBig);
}

TEST(GpuMapping, IndependentAlignedPhysicalChunksCanShareOneRun)
{
	auto physical = pages(64);
	for (unsigned i = 32; i < physical.size(); ++i)
		physical[i] += 16 * kBig;
	const auto runs = plan(physical, kVa, 0, physical.size() * kPage);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 17);
	EXPECT_EQ(runs[0].bytes, 2 * kBig);
}

TEST(GpuMapping, TruncatedPageVectorCannotFormABigPage)
{
	const auto physical = pages(31);
	EXPECT_FALSE(switch_gpu_mapping_big_page(physical.data(), 31, 0));
	EXPECT_FALSE(switch_gpu_mapping_big_page(physical.data(), 31, 31));
	EXPECT_FALSE(switch_gpu_mapping_big_page(physical.data(), 31, UINT32_MAX));
	const auto runs = plan(physical, kVa, 0, physical.size() * kPage);
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].shift, 12);
}

TEST(GpuMapping, InvalidAndOverflowingExtentsAreRejected)
{
	const auto physical = pages(32);
	switch_gpu_mapping_run run{};
	for (const auto extent :
	     std::array<std::array<uint64_t, 3>, 7>{{{kVa, 0, 0},
						     {kVa + 1, 0, kPage},
						     {kVa, 1, kPage},
						     {kVa, 0, kPage + 1},
						     {kVa, kBig, kPage},
						     {kVa, 0, kBig + kPage},
						     {UINT64_MAX - kPage + 1, 0, kPage}}})
		EXPECT_FALSE(switch_gpu_mapping_next(physical.data(), 32, extent[0], extent[1],
						     extent[2], &run));
	EXPECT_FALSE(switch_gpu_mapping_next(nullptr, 32, kVa, 0, kPage, &run));
	EXPECT_FALSE(switch_gpu_mapping_next(physical.data(), 32, kVa, 0, kPage, nullptr));
}

TEST(GpuMapping, EverySubrangeCoversExactlyItsOwnedPages)
{
	for (unsigned pattern = 0; pattern < 3; ++pattern) {
		auto physical = pages(128);
		if (pattern == 1)
			physical[45] += 16 * kBig;
		if (pattern == 2)
			for (unsigned i = 64; i < 96; ++i)
				physical[i] += 32 * kBig;
		for (unsigned start = 0; start < physical.size(); ++start) {
			for (unsigned count = 1; count <= physical.size() - start; ++count) {
				for (unsigned phase : {0u, 1u, 17u, 31u}) {
					uint64_t va = kVa + phase * kPage;
					uint64_t offset = start * kPage;
					const auto runs = plan(physical, va, offset, count * kPage);
					ASSERT_FALSE(runs.empty());
					uint64_t total = 0;
					for (const auto &run : runs) {
						const uint64_t unit = uint64_t{1} << run.shift;
						ASSERT_TRUE(run.shift == 12 || run.shift == 17);
						ASSERT_EQ((va | offset | run.bytes) & (unit - 1),
							  0u);
						ASSERT_LE(run.bytes, (count * kPage) - total);
						if (run.shift == 17) {
							for (uint64_t at = 0; at < run.bytes;
							     at += kBig) {
								const size_t first =
									(offset + at) / kPage;
								ASSERT_LE(first + 32,
									  physical.size());
								EXPECT_EQ(physical[first] &
										  (kBig - 1),
									  0u);
								for (unsigned page = 0; page < 32;
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
	const auto physical = pages(64, 0x80010000);
	const auto runs = plan(physical, kVa, 16 * kPage, 48 * kPage);
	ASSERT_EQ(runs.size(), 2u);
	EXPECT_EQ(runs[0].shift, 17);
	EXPECT_EQ(runs[0].bytes, kBig);
	EXPECT_EQ(runs[1].shift, 12);
	EXPECT_EQ(runs[1].bytes, 16 * kPage);
}

// LIMITS C5: a 720p present surface is 3,686,400 bytes. Contiguous and 128 KiB-aligned, 28 128 KiB PTEs could map all
// but its last 4 KiB pages; scattered, or at a VA off the 128 KiB grid, none.
TEST(GpuMapping, BigBytesCountsWhat128KiBPtesCouldMap)
{
	constexpr uint64_t kSurface = 3686400, kBig = 128 * 1024;
	const uint32_t count = uint32_t((kSurface + 4095) / 4096);
	std::vector<uint64_t> pages(count);
	for (uint32_t i = 0; i < count; ++i)
		pages[i] = 0x80000000ull + uint64_t(i) * 4096;
	EXPECT_EQ(switch_gpu_mapping_big_bytes(pages.data(), count, 0x40000000, 0, kSurface),
		  28 * kBig);
	EXPECT_EQ(switch_gpu_mapping_big_bytes(pages.data(), count, 0x40001000, 0, kSurface - 4096),
		  0u);
	for (uint32_t i = 0; i < count; i += 2)
		pages[i] += 0x100000000ull;
	EXPECT_EQ(switch_gpu_mapping_big_bytes(pages.data(), count, 0x40000000, 0, kSurface), 0u);
}
