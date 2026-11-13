// SPDX-License-Identifier: MIT
// TegraGM20BViewMerge.h: the unlock of a CPU write lock during which the GPU wrote the surface keeps both writers' bytes.
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BViewMerge.h"

namespace
{
// F-v1, gpu.path.resource_ready: the CPU locked for writing and wrote nothing; the GPU cleared the surface green.
TEST(ViewMerge, ACpuThatWroteNothingKeepsEveryGpuByte)
{
	std::vector<uint64_t> base(512, 0), rows(512, 0), gpu(512, 0xff00ff00ff00ff00ull);
	tgm_view_merge(gpu.data(), rows.data(), base.data(), gpu.size());
	for (uint64_t w : gpu)
		EXPECT_EQ(w, 0xff00ff00ff00ff00ull);
}

TEST(ViewMerge, AGpuThatWroteNothingKeepsEveryCpuByte)
{
	std::vector<uint64_t> base(64, 0x1111111111111111ull), rows(base), gpu(base);
	rows[3] = 0x2222222222222222ull;
	rows[9] = 0x1111111111112211ull;
	tgm_view_merge(gpu.data(), rows.data(), base.data(), gpu.size());
	EXPECT_EQ(gpu, rows);
}

TEST(ViewMerge, DisjointBytesOfOneWordKeepBothWriters)
{
	const uint64_t base = 0x0000000000000000ull;
	const uint64_t rows = 0x00000000000000abull;  // the CPU wrote byte 0
	uint64_t gpu = 0xcd00000000000000ull;         // the GPU wrote byte 7
	tgm_view_merge(&gpu, &rows, &base, 1);
	EXPECT_EQ(gpu, 0xcd000000000000abull);
}

TEST(ViewMerge, AByteBothWroteIsTheCpus)
{
	const uint64_t base = 0x0101010101010101ull;
	const uint64_t rows = 0x0101010101017f01ull;
	uint64_t gpu = 0x0202020202020202ull;
	tgm_view_merge(&gpu, &rows, &base, 1);
	EXPECT_EQ(gpu, 0x0202020202027f02ull);
}

TEST(ViewMerge, AChangeInAnyBitOfAByteTakesTheWholeByte)
{
	for (uint32_t bit = 0; bit < 64; bit++) {
		const uint64_t base = 0, rows = 1ull << bit;
		uint64_t gpu = ~0ull;
		tgm_view_merge(&gpu, &rows, &base, 1);
		const uint64_t byte = 0xffull << (bit / 8 * 8);
		EXPECT_EQ(gpu, (~0ull & ~byte) | rows) << bit;
	}
}
}  // namespace
