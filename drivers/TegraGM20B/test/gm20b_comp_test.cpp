// SPDX-License-Identifier: MIT
// TegraGM20BComp.h against nvgpu's gm20b compression code (L4T r32.7.6), transcribed below as the reference: the compbit
// backing store's size and line count (gm20b_ltc_init_comptags), its CBC_BASE (gm20b_ltc_init_cbc with
// gm20b_ltc_cbc_fix_config), and the PTE fields of a compressed bind (gk20a/mm_gk20a.c __update_pte with hw_gmmu_gm20b.h).
// GM20B has one LTC of two slices (pri_ringmaster_enum_ltc; gm20b_determine_L2_size_bytes' lts_per_ltc) and the Switch
// 4 GiB of RAM. CBC_PARAM's other fields are read from the GPU at boot; no published GM20B readout exists, so every
// plausible comptags-per-cacheline and cache line size is checked.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BComp.h"

namespace
{
constexpr uint32_t kLtcs = 1, kSlices = 2;
constexpr uint64_t kRamMib = 4096;

uint32_t param(uint32_t per_line, uint32_t line_code, uint32_t slices = kSlices)
{
	return per_line | line_code << 24 | slices << 28;
}

struct NvgpuCbc {
	uint32_t lines;
	uint64_t bytes;
};

// gm20b_ltc_init_comptags
NvgpuCbc nvgpuInitComptags(uint32_t cbc_param, uint32_t ltc_count, uint32_t max_size_mib)
{
	const uint32_t hw_max = 0x1ffff;
	const uint32_t per_line = cbc_param & 0xffff;
	const uint32_t cacheline_size = 512u << ((cbc_param >> 24) & 0xf);
	const uint32_t slices_per_ltc = cbc_param >> 28;
	uint32_t max_comptag_lines = max_size_mib << 3;
	if (max_comptag_lines > hw_max)
		max_comptag_lines = hw_max;
	uint64_t size = (uint64_t)((max_comptag_lines + per_line - 1) / per_line) * cacheline_size *
			slices_per_ltc * ltc_count;
	size += (uint64_t)ltc_count << 11;
	size = (size + 64 * 1024 - 1) / (64 * 1024) * (64 * 1024);
	max_comptag_lines = (uint32_t)(size * per_line /
				       ((uint64_t)cacheline_size * slices_per_ltc * ltc_count));
	if (max_comptag_lines > hw_max)
		max_comptag_lines = hw_max;
	return {max_comptag_lines, size};
}

// gm20b_ltc_init_cbc and gm20b_ltc_cbc_fix_config
uint32_t nvgpuCbcBase(uint64_t iova, uint32_t ltc_count)
{
	uint64_t post_divide = (iova >> 11) / ltc_count;
	if (((post_divide * ltc_count) << 11) < iova)
		post_divide++;
	return ltc_count == 2 ? (uint32_t)post_divide * 2 : (uint32_t)post_divide;
}

TEST(CbcParam, Fields)
{
	const uint32_t p = 0x2201000cu;
	EXPECT_EQ(tgm_cbc_comptags_per_cacheline(p), 12u);
	EXPECT_EQ(tgm_cbc_cacheline_bytes(p), 2048u);
	EXPECT_EQ(tgm_cbc_slices(p), 2u);
	EXPECT_EQ(tgm_cbc_cacheline_bytes(0), 512u);
}

TEST(CbcSize, MatchesNvgpuAndCoversAllOfRam)
{
	for (uint32_t per_line : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
		for (uint32_t code : {0u, 1u, 2u, 3u}) {
			SCOPED_TRACE(testing::Message()
				     << per_line << " lines per " << (512u << code) << " B");
			const uint32_t p = param(per_line, code);
			const uint64_t row = (uint64_t)(512u << code) * kSlices * kLtcs;
			const NvgpuCbc ref = nvgpuInitComptags(p, kLtcs, kRamMib);
			struct tgm_cbc c = {};
			ASSERT_TRUE(tgm_cbc_size(p, kLtcs, kRamMib, 17, &c));
			EXPECT_EQ(c.bytes, ref.bytes);
			EXPECT_EQ(c.bytes % 0x10000, 0u);
			EXPECT_LE(c.lines, ref.lines);
			EXPECT_GE(c.lines, kRamMib << 3);
			// the last line's cacheline row ends inside the store even from a base rounded up by the slack
			const uint64_t used = (c.lines + per_line - 1) / per_line * row;
			EXPECT_LE(used + ((uint64_t)kLtcs << 11), c.bytes);
		}
	}
}

TEST(CbcSize, FullCompTagLineLeavesSixteenBits)
{
	struct tgm_cbc c = {};
	ASSERT_TRUE(tgm_cbc_size(param(4, 0), kLtcs, 16384, 16, &c));
	EXPECT_EQ(c.lines, 0xffffu);
	ASSERT_TRUE(tgm_cbc_size(param(4, 0), kLtcs, 1ULL << 20, 17, &c));
	EXPECT_EQ(c.lines, 0x1ffffu);
	ASSERT_TRUE(tgm_cbc_size(param(4, 0), kLtcs, kRamMib, 16, &c));
	EXPECT_GE(c.lines, (uint32_t)(kRamMib << 3));
	EXPECT_LE(c.lines, 0xffffu);
}

TEST(CbcSize, NoCompressionWithoutParameters)
{
	struct tgm_cbc c = {};
	EXPECT_FALSE(tgm_cbc_size(param(0, 2), kLtcs, kRamMib, 16, &c));
	EXPECT_FALSE(tgm_cbc_size(param(4, 2, 0), kLtcs, kRamMib, 16, &c));
	EXPECT_FALSE(tgm_cbc_size(param(4, 2), 0, kRamMib, 16, &c));
	EXPECT_FALSE(tgm_cbc_size(param(4, 2), kLtcs, 0, 16, &c));
}

TEST(CbcBase, MatchesNvgpu)
{
	for (uint32_t ltcs : {1u, 2u}) {
		for (uint64_t pa :
		     {0xe4000000ULL, 0xe4001000ULL, 0xe4000800ULL, 0xe4000801ULL, 0x1fffff000ULL}) {
			uint32_t base = 0;
			ASSERT_TRUE(tgm_cbc_base(pa, ltcs, &base));
			EXPECT_EQ(base, nvgpuCbcBase(pa, ltcs)) << std::hex << pa << " " << ltcs;
		}
	}
	uint32_t base = 0;
	ASSERT_TRUE(tgm_cbc_base(0xe4000000ULL, 1, &base));
	EXPECT_EQ(base, 0xe4000000u >> 11);
	EXPECT_FALSE(tgm_cbc_base(1ULL << 37, 1, &base));
	EXPECT_FALSE(tgm_cbc_base(0xe4000000ULL, 0, &base));
}

std::vector<uint64_t> runs(std::initializer_list<uint64_t> starts)
{
	std::vector<uint64_t> pas;
	for (uint64_t start : starts)
		for (uint64_t i = 0; i < TGM_COMP_BYTES >> 12; i++)
			pas.push_back(start + (i << 12));
	return pas;
}

TEST(PageShift, BigPagesOnlyOverAlignedRuns)
{
	auto pas = runs({0xe4020000, 0x180000000, 0xe4000000});
	EXPECT_EQ(tgm_comp_page_shift(pas.data(), (uint32_t)pas.size()), (uint32_t)TGM_COMP_SHIFT);

	auto misaligned = runs({0xe4021000});
	EXPECT_EQ(tgm_comp_page_shift(misaligned.data(), (uint32_t)misaligned.size()), 12u);

	auto broken = runs({0xe4020000, 0xe4040000});
	broken[40] += 0x1000;
	EXPECT_EQ(tgm_comp_page_shift(broken.data(), (uint32_t)broken.size()), 12u);

	auto partial = runs({0xe4020000});
	EXPECT_EQ(tgm_comp_page_shift(partial.data(), (uint32_t)partial.size() - 1), 12u);
	EXPECT_EQ(tgm_comp_page_shift(partial.data(), 0), 12u);
}

// The GPU pool as shim/linux.c keeps it: 256 MiB of 4 KiB pages, a bit each in the map (taken) and in dirty (free, not
// known to be zero)
struct Pool {
	static constexpr uint64_t kPages = (256ULL << 20) >> 12;
	static constexpr uint64_t kWord = sizeof(unsigned long) * 8;
	std::vector<unsigned long> map = std::vector<unsigned long>(kPages / kWord),
				   dirty = std::vector<unsigned long>(kPages / kWord);

	static void set(std::vector<unsigned long> &bits, uint64_t page)
	{
		bits[page / kWord] |= 1UL << (page % kWord);
	}

	static bool test(const std::vector<unsigned long> &bits, uint64_t page)
	{
		return (bits[page / kWord] >> (page % kWord)) & 1;
	}

	int pick(uint32_t count, std::vector<uint64_t> &chunks) const
	{
		chunks.assign(count, ~0ULL);
		return tgm_comp_pick_chunks(map.data(), dirty.data(), kPages, count, chunks.data());
	}
};

// Every picked chunk is free and distinct, and the pages the caller then zeroes are the dirty chunks' alone
void expectFree(const Pool &p, const std::vector<uint64_t> &chunks, int dirty)
{
	std::set<uint64_t> seen;
	for (size_t i = 0; i < chunks.size(); i++) {
		bool anyDirty = false;
		for (uint64_t k = 0; k < TGM_COMP_PAGES; k++) {
			EXPECT_FALSE(Pool::test(p.map, chunks[i] * TGM_COMP_PAGES + k))
				<< chunks[i];
			anyDirty = anyDirty || Pool::test(p.dirty, chunks[i] * TGM_COMP_PAGES + k);
		}
		EXPECT_EQ(anyDirty, i >= chunks.size() - (size_t)dirty) << i;
		EXPECT_TRUE(seen.insert(chunks[i]).second) << chunks[i];
	}
}

TEST(PoolChunks, AFragmentedPoolServesWhatItHasFree)
{
	Pool p;
	// every chunk but every 16th holds a taken page: 128 of 2048 chunks free, none next to another
	for (uint64_t c = 0; c < Pool::kPages / TGM_COMP_PAGES; c++)
		if (c % 16)
			Pool::set(p.map, c * TGM_COMP_PAGES + (c % TGM_COMP_PAGES));
	std::vector<uint64_t> chunks;
	const int dirty = p.pick(57, chunks);  // a 720x1280 RGBA16F target: 7296 KiB
	ASSERT_EQ(dirty, 0);
	expectFree(p, chunks, dirty);
	EXPECT_EQ(chunks.front(), 2032u);  // the top free chunk first
	for (uint64_t c : chunks)
		EXPECT_EQ(c % 16, 0u);
	ASSERT_EQ(p.pick(128, chunks), 0);
	expectFree(p, chunks, 0);
	EXPECT_EQ(p.pick(129, chunks), -1);
}

TEST(PoolChunks, CleanChunksBeforeDirtyOnes)
{
	Pool p;
	// the top 64 chunks are free and dirty (a page each), the rest free and clean
	const uint64_t chunks_total = Pool::kPages / TGM_COMP_PAGES;
	for (uint64_t c = chunks_total - 64; c < chunks_total; c++)
		Pool::set(p.dirty, c * TGM_COMP_PAGES + 7);
	std::vector<uint64_t> chunks;
	ASSERT_EQ(p.pick(10, chunks), 0);
	expectFree(p, chunks, 0);
	EXPECT_EQ(chunks.front(), chunks_total - 65);
	// only clean chunks below the first taken page of the pool: dirty ones make up the rest
	for (uint64_t c = 0; c < chunks_total - 64 - 3; c++)
		Pool::set(p.map, c * TGM_COMP_PAGES);
	const int dirty = p.pick(10, chunks);
	ASSERT_EQ(dirty, 7);
	expectFree(p, chunks, dirty);
}

TEST(PoolChunks, ChunksStraddleNoWord)
{
	Pool p;
	Pool::set(p.map, 31);      // chunk 0's last page
	Pool::set(p.map, 32 * 5);  // chunk 5's first page
	for (uint64_t c = 7; c < Pool::kPages / TGM_COMP_PAGES; c++)
		Pool::set(p.map, c * TGM_COMP_PAGES + 16);
	std::vector<uint64_t> chunks;
	ASSERT_EQ(p.pick(5, chunks), 0);
	EXPECT_EQ(chunks.front(), 6u);
	std::sort(chunks.begin(), chunks.end());
	EXPECT_EQ(chunks, (std::vector<uint64_t>{1, 2, 3, 4, 6}));
	EXPECT_EQ(p.pick(6, chunks), -1);
}

// The kernel picks under the pool's spinlock: one pass over 2048 chunks, however fragmented the pool, in microseconds.
TEST(PoolChunks, OnePassBoundsThePick)
{
	Pool p;
	for (uint64_t c = 1; c < Pool::kPages / TGM_COMP_PAGES; c++)
		Pool::set(p.map, c * TGM_COMP_PAGES + 31);
	std::vector<uint64_t> chunks;
	const auto start = std::chrono::steady_clock::now();
	constexpr int kPicks = 100;
	for (int i = 0; i < kPicks; i++)
		ASSERT_EQ(p.pick(2, chunks),
			  -1);  // the worst case: every chunk looked at, one found
	const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - start)
				.count() /
			kPicks;
	RecordProperty("pick_us", static_cast<int>(us));
	EXPECT_LT(us, 1000);
	ASSERT_EQ(p.pick(1, chunks), 0);
	EXPECT_EQ(chunks.front(), 0u);
}

TEST(BindOk, WholeBigPagesOfBigPageMemory)
{
	const uint64_t big = TGM_COMP_BYTES;
	EXPECT_TRUE(tgm_comp_bind_ok(0x100000000ULL, big, 4 * big, TGM_COMP_SHIFT));
	EXPECT_FALSE(tgm_comp_bind_ok(0x100000000ULL, big, 4 * big, 12));
	EXPECT_FALSE(tgm_comp_bind_ok(0x100010000ULL, 0, big, TGM_COMP_SHIFT));
	EXPECT_FALSE(tgm_comp_bind_ok(0x100000000ULL, 0x10000, big, TGM_COMP_SHIFT));
	EXPECT_FALSE(tgm_comp_bind_ok(0x100000000ULL, 0, big + 0x1000, TGM_COMP_SHIFT));
	EXPECT_FALSE(tgm_comp_bind_ok(0x100000000ULL, 0, 0, TGM_COMP_SHIFT));
}

// hw_gmmu_gm20b.h: word 1 of a PTE
uint32_t nvgpuComptaglineF(uint32_t v)
{
	return (v & 0x1ffffu) << 12;
}

uint32_t nvgpuKindF(uint32_t v)
{
	return (v & 0xffu) << 4;
}

TEST(Pte, FieldsAreNvgpus)
{
	for (uint32_t line : {1u, 2u, 0x1234u, 0xffffu, 0x1ffffu})
		EXPECT_EQ(tgm_pte_comptagline(line) >> 32, nvgpuComptaglineF(line));
	for (uint32_t kind : {0x00u, 0xdbu, 0xfeu, 0x11u})
		EXPECT_EQ(tgm_pte_kind(kind) >> 32, nvgpuKindF(kind));
	EXPECT_EQ(tgm_pte_comptagline(1) | tgm_pte_kind(0xff), 0x1ff000000000ULL);
}

// A compressed bind as nvkm writes it (vmmgf100.c: gf100_vmm_valid, then one PTE per 128 KiB page with the line moved on
// by tgm_pte_comptagline(1)) next to nvgpu's (ctag_offset = comptags.offset + phys_offset / 128 KiB, the line advancing
// with each page of 128 KiB, and the full-comp-tag-line top bit set only for a page at an odd 64 KiB).
TEST(Pte, BindLinesFollowNvgpu)
{
	const uint64_t first = 0x40, offset = 3 * TGM_COMP_BYTES, pa = 0xe4100000;
	const uint32_t kind = 0xdb;
	uint64_t type = tgm_pte_kind(kind) | tgm_pte_comptagline(tgm_comp_line(first, offset));
	uint64_t ctag = (first + (offset >> 17)) << 17;
	for (uint32_t i = 0; i < 8; i++) {
		const uint64_t page_pa = pa + i * TGM_COMP_BYTES;
		const uint64_t pte = (page_pa >> 8) | type;
		type += tgm_pte_comptagline(1);

		uint32_t word1 = nvgpuKindF(kind) | nvgpuComptaglineF((uint32_t)(ctag >> 17));
		if (page_pa & 0x10000)
			word1 |= nvgpuComptaglineF(1u << 16);
		ctag += TGM_COMP_BYTES;

		EXPECT_EQ((uint32_t)(pte >> 32), word1) << i;
		EXPECT_EQ((uint32_t)pte, (uint32_t)(page_pa >> 12) << 4) << i;
		EXPECT_EQ(pte & (1ULL << 60), 0u) << i;
	}
}
}  // namespace
