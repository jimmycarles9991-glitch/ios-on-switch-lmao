// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuSurfacePages.h"

namespace
{
struct Memory {
	uint64_t bytes, native = 16384;
	uint32_t reads = 0, outside = 0;

	uint64_t physical(uint64_t at) const
	{
		return 0x80000000 + (at / native) * 3 * native + at % native;
	}

	uint64_t read(uint64_t at, uint64_t &length)
	{
		++reads;
		if (at >= bytes) {
			++outside;
			return 0;
		}
		length = std::min(native - at % native, bytes - at);
		return physical(at);
	}

	std::vector<uint64_t> pages(uint64_t base, uint64_t length) const
	{
		std::vector<uint64_t> out;
		for (uint64_t at = 0; at < length; at += 4096)
			out.push_back(physical(base + at));
		return out;
	}

	SwitchGpuSurfaceMatch match(uint64_t base, uint64_t allocation,
				    const std::vector<uint64_t> &pages)
	{
		const auto result = switch_gpu_surface_page_vector(
			bytes, base, allocation, native, pages.data(), uint32_t(pages.size()),
			[&](uint64_t at, uint64_t &length) {
				return read(at, length);
			});
		EXPECT_EQ(outside, 0u);
		return result;
	}
};
}

TEST(SurfacePages, AttestsOddSurfaceNativePageTailsWithoutReadingPastTheDescriptor)
{
	for (uint64_t bytes : {uint64_t(1344 * 77), uint64_t(72 * 72 * 8), uint64_t(136 * 136 * 8),
			       uint64_t(188672), uint64_t(49280)}) {
		Memory memory{bytes};
		const uint64_t mapped = (bytes + 16383) & ~uint64_t(16383);
		const auto pages = memory.pages(0, mapped);
		EXPECT_EQ(memory.match(0, bytes, pages), SwitchGpuSurfaceMatch::Match) << bytes;
		EXPECT_LE(memory.reads, (bytes + 16383) / 16384 + 1);
	}
}

TEST(SurfacePages, AlignedSurfaceAndLegalPrefixStillMatch)
{
	Memory memory{32768};
	auto pages = memory.pages(0, 32768);
	EXPECT_EQ(memory.match(0, 32768, pages), SwitchGpuSurfaceMatch::Match);
	pages.resize(4);
	EXPECT_EQ(memory.match(0, 32768, pages), SwitchGpuSurfaceMatch::Match);
}

TEST(SurfacePages, RejectsTailFromADifferentNativePage)
{
	Memory memory{103488};
	auto pages = memory.pages(0, 114688);
	pages.back() += 16384;
	EXPECT_EQ(memory.match(0, memory.bytes, pages), SwitchGpuSurfaceMatch::Pages);
}

TEST(SurfacePages, RejectsImportBeyondTheOwnedNativePage)
{
	Memory odd{103488};
	EXPECT_EQ(odd.match(0, odd.bytes, odd.pages(0, 118784)), SwitchGpuSurfaceMatch::Padding);
	Memory aligned{32768};
	EXPECT_EQ(aligned.match(0, aligned.bytes, aligned.pages(0, 36864)),
		  SwitchGpuSurfaceMatch::Padding);
}

TEST(SurfacePages, RejectsAnotherSurfaceOrPlaneSubspan)
{
	Memory memory{32768};
	auto pages = memory.pages(0, 32768);
	pages.front() += 16384;
	EXPECT_EQ(memory.match(0, memory.bytes, pages), SwitchGpuSurfaceMatch::Pages);
	EXPECT_EQ(memory.match(0, memory.bytes, memory.pages(16384, 16384)),
		  SwitchGpuSurfaceMatch::Pages);
}

TEST(SurfacePages, DerivesPaddingFromThePhysicalPageAtANonzeroDescriptorOffset)
{
	Memory memory{4096 + 17000};
	auto pages = memory.pages(4096, 28672);
	EXPECT_EQ(memory.match(4096, 17000, pages), SwitchGpuSurfaceMatch::Match);
	pages.push_back(memory.physical(32768));
	EXPECT_EQ(memory.match(4096, 17000, pages), SwitchGpuSurfaceMatch::Padding);
}

TEST(SurfacePages, HandlesFourKiBNativePages)
{
	Memory memory{103488, 4096};
	EXPECT_EQ(memory.match(0, memory.bytes, memory.pages(0, 106496)),
		  SwitchGpuSurfaceMatch::Match);
	EXPECT_EQ(memory.match(0, memory.bytes, memory.pages(0, 110592)),
		  SwitchGpuSurfaceMatch::Padding);
}

TEST(SurfacePages, RejectsInvalidExtentsWithoutAnyDescriptorRead)
{
	Memory memory{16384};
	const auto pages = memory.pages(0, 16384);
	for (const auto extent :
	     {std::pair<uint64_t, uint64_t>{1, 16}, {16384, 1}, {0, 0}, {4096, UINT64_MAX}})
		EXPECT_EQ(memory.match(extent.first, extent.second, pages),
			  SwitchGpuSurfaceMatch::Extent);
	EXPECT_EQ(memory.reads, 0u);
}
