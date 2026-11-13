// SPDX-License-Identifier: MIT
// TegraGM20BRelayout.h on the host: its pushes run through a model of MAXWELL_DMA_COPY_A (the methods the kernel's moves
// use: remapped copies of components, pitch and block-linear sides, NON_PIPELINED launches) over modelled GPU memory, and
// every move must leave exactly the bytes TegraGM20BLayout.h says: the texels a move to rows writes are the rows the CPU
// copy (tgm_layout_read) gives, a move to tiled writes what tgm_layout_write does, a tile in place turns a surface's rows
// into its texels, and nothing outside the texels changes. The acquires come first, one per writer, as the host reads them.
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BLayout.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.h"
#include "gm20b_ce_model.h"

namespace
{
std::vector<uint8_t> pattern(size_t n, uint32_t seed)
{
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; i++)
		v[i] = uint8_t(i * 2654435761u >> 13 ^ seed);
	return v;
}

constexpr uint64_t kSurfaceVa = 1ull << 32, kSurfaceRowsVa = 2ull << 32, kRowsVa = 3ull << 32;

struct Case {
	uint32_t w, h, bpp, pad;
};

const Case kCases[] = {{1, 1, 4, 0},     {3, 5, 1, 13},    {64, 64, 4, 0},    {65, 87, 2, 6},
		       {100, 60, 4, 64}, {127, 129, 8, 8}, {300, 200, 16, 0}, {45, 18, 4, 4},
		       {33, 43, 1, 3},   {86, 86, 4, 0},   {85, 85, 4, 0},    {11, 11, 2, 0},
		       {1, 300, 4, 0},   {520, 22, 8, 0}};

// The surface's pages, viewed twice as the kernel maps them, and the scratch: one region each for the model
struct Surface {
	tgm_layout l{};
	uint32_t bytesPerRow = 0;
	std::vector<uint8_t> surface, rows;
	Memory memory;

	explicit Surface(const Case &c)
	{
		EXPECT_TRUE(tgm_layout_tiled(c.w, c.h, c.bpp, &l));
		bytesPerRow = c.w * c.bpp + c.pad;
		surface.assign(std::max<uint64_t>(l.bytes, uint64_t(bytesPerRow) * c.h), 0);
		rows.assign((uint64_t(bytesPerRow) * c.h + 4095) & ~4095ull, 0);
		memory.map(kSurfaceVa, surface);
		memory.map(kSurfaceRowsVa, surface);
		memory.map(kRowsVa, rows);
	}

	void move(tgm_relayout_op op, const std::vector<tgm_relayout_wait> &waits = {})
	{
		CopyEngine ce(memory);
		uint32_t w[TGM_RELAYOUT_DWORDS];
		const tgm_relayout_move m = {kSurfaceVa, kSurfaceRowsVa, kRowsVa, bytesPerRow,
					     op,         rows.size(),    0};
		const uint32_t n =
			tgm_relayout_push(w, &l, &m, waits.data(), uint32_t(waits.size()));
		ASSERT_LE(n, TGM_RELAYOUT_DWORDS);
		ce.run(w, n);
		ASSERT_FALSE(ce.fault);
		ASSERT_EQ(ce.acquires.size(), waits.size());
		for (size_t i = 0; i < waits.size(); i++)
			EXPECT_EQ(ce.acquires[i], std::make_pair(waits[i].va, waits[i].seqno));
	}
};

TEST(Relayout, ToRowsIsTheCpuDetile)
{
	for (const Case &c : kCases) {
		Surface s(c);
		s.surface = pattern(s.surface.size(), c.w);
		s.rows = pattern(s.rows.size(), 0x5a);  // an earlier user's bytes: none may show
		std::vector<uint8_t> want(s.rows.size(), 0);
		tgm_layout_read(&s.l, s.surface.data(), 0, 0, c.w, c.h, want.data(), s.bytesPerRow);
		s.move(TGM_RELAYOUT_TO_ROWS);
		ASSERT_EQ(s.rows, want) << c.w << "x" << c.h << " bpp " << c.bpp;
	}
}

TEST(Relayout, ToTiledIsTheCpuTileAndTouchesNothingElse)
{
	for (const Case &c : kCases) {
		Surface s(c);
		s.rows = pattern(s.rows.size(), c.h);
		std::vector<uint8_t> want = pattern(s.surface.size(), 99);
		s.surface = want;
		tgm_layout_write(&s.l, want.data(), 0, 0, c.w, c.h, s.rows.data(), s.bytesPerRow);
		s.move(TGM_RELAYOUT_TO_TILED);
		ASSERT_EQ(s.surface, want) << c.w << "x" << c.h << " bpp " << c.bpp;
	}
}

TEST(Relayout, TileInPlaceTurnsTheSurfacesRowsIntoItsTexels)
{
	for (const Case &c : kCases) {
		Surface s(c);
		const std::vector<uint8_t> held = pattern(s.surface.size(), 7);
		s.surface = held;
		std::vector<uint8_t> texels(s.surface.size(), 0);
		tgm_layout_write(&s.l, texels.data(), 0, 0, c.w, c.h, held.data(), s.bytesPerRow);
		s.move(TGM_RELAYOUT_TILE_IN_PLACE);
		std::vector<uint8_t> back(s.rows.size(), 0);
		tgm_layout_read(&s.l, s.surface.data(), 0, 0, c.w, c.h, back.data(), s.bytesPerRow);
		for (uint32_t y = 0; y < c.h; y++)
			ASSERT_EQ(0, memcmp(&back[y * s.bytesPerRow], &held[y * s.bytesPerRow],
					    size_t(c.w) * c.bpp))
				<< c.w << "x" << c.h << " bpp " << c.bpp << " row " << y;
	}
}

TEST(Relayout, UntileInPlaceGivesTheSurfaceItsRowsBack)
{
	for (const Case &c : kCases) {
		Surface s(c);
		const std::vector<uint8_t> rows = pattern(s.rows.size(), 11);
		tgm_layout_write(&s.l, s.surface.data(), 0, 0, c.w, c.h, rows.data(),
				 s.bytesPerRow);
		s.move(TGM_RELAYOUT_UNTILE_IN_PLACE);
		for (uint32_t y = 0; y < c.h; y++)
			ASSERT_EQ(0, memcmp(&s.surface[y * s.bytesPerRow], &rows[y * s.bytesPerRow],
					    size_t(c.w) * c.bpp))
				<< c.w << "x" << c.h << " bpp " << c.bpp << " row " << y;
	}
}

TEST(Relayout, RoundTripIsIdentity)
{
	for (const Case &c : kCases) {
		Surface s(c);
		const std::vector<uint8_t> texels = pattern(s.surface.size(), 3);
		s.surface = texels;
		s.move(TGM_RELAYOUT_TO_ROWS);
		std::fill(s.surface.begin(), s.surface.end(), 0);
		s.move(TGM_RELAYOUT_TO_TILED);
		for (uint32_t y = 0; y < c.h; y++)
			for (uint32_t xB = 0; xB < c.w * c.bpp; xB++) {
				const uint64_t at = tgm_layout_offset(&s.l, xB, y);
				ASSERT_EQ(s.surface[at], texels[at])
					<< c.w << "x" << c.h << " bpp " << c.bpp;
			}
	}
}

// A tiling move follows the surface's running writers in its own push: a connection's channel and the copy engine's (whose
// fence is in every space's fence view, gpu_chan.c tgm_fence_kernel) are semaphore acquires ahead of its copies; its own
// channel's work needs none; a writer with no fence in the view, or past the push's acquires, is refused, never waited for.
TEST(Relayout, TheMovesPushCarriesItsWritersAcquires)
{
	constexpr uint32_t kSelf = 3;
	constexpr uint64_t kUserFence = 0x8000001040, kCopyEngineFence = 0x8000100000;
	const tgm_relayout_writer writers[] = {{kSelf, 9}, {5, 7}, {6, 2}, {8, 4}};
	const uint64_t fenceVa[] = {0x8000003000, kUserFence, kCopyEngineFence, 0};
	tgm_relayout_wait waits[TGM_RELAYOUT_MAX_WAITS];
	uint32_t refused = 0;
	const uint32_t n = tgm_relayout_follow(kSelf, writers, fenceVa, 4, waits, 0, &refused);
	ASSERT_EQ(n, 2u);
	EXPECT_EQ(refused, 1u);
	Surface s(kCases[2]);
	s.move(TGM_RELAYOUT_TILE_IN_PLACE, {waits, waits + n});
	EXPECT_EQ(waits[0].va, kUserFence);
	EXPECT_EQ(waits[0].seqno, 7u);
	EXPECT_EQ(waits[0].uid, 5u);
	EXPECT_EQ(waits[1].va, kCopyEngineFence);
	EXPECT_EQ(waits[1].seqno, 2u);
	EXPECT_EQ(waits[1].uid, 6u);
}

TEST(Relayout, WritersPastThePushsAcquiresAreRefused)
{
	tgm_relayout_writer writers[TGM_RELAYOUT_MAX_WAITS + 2];
	uint64_t fenceVa[TGM_RELAYOUT_MAX_WAITS + 2];
	for (uint32_t i = 0; i < TGM_RELAYOUT_MAX_WAITS + 2; i++) {
		writers[i] = {10 + i, i};
		fenceVa[i] = 0x9000000000 + 16 * i;
	}
	tgm_relayout_wait waits[TGM_RELAYOUT_MAX_WAITS];
	uint32_t refused = 0;
	const uint32_t n = tgm_relayout_follow(1, writers, fenceVa, TGM_RELAYOUT_MAX_WAITS + 2,
					       waits, 0, &refused);
	EXPECT_EQ(n, TGM_RELAYOUT_MAX_WAITS);
	EXPECT_EQ(refused, 2u);
	Surface s(kCases[2]);
	s.move(TGM_RELAYOUT_TO_ROWS, {waits, waits + n});
}

Memory *cpuMemory;

uint8_t *cpuPage(uint64_t page)
{
	return cpuMemory->at(page);
}

TEST(Relayout, CheckpointMakesPartialInPlaceOverwriteRecoverable)
{
	for (const auto operation : {TGM_RELAYOUT_TILE_IN_PLACE, TGM_RELAYOUT_UNTILE_IN_PLACE}) {
		Surface s({64, 64, 4, 64});
		s.surface = pattern(s.surface.size(), 27);
		if (operation == TGM_RELAYOUT_UNTILE_IN_PLACE)
			s.move(TGM_RELAYOUT_TILE_IN_PLACE);
		const auto before = s.surface;
		auto expected = before;
		if (operation == TGM_RELAYOUT_TILE_IN_PLACE)
			tgm_layout_write(&s.l, expected.data(), 0, 0, 64, 64, before.data(),
					 s.bytesPerRow);
		else
			tgm_layout_read(&s.l, before.data(), 0, 0, 64, 64, expected.data(),
					s.bytesPerRow);
		std::vector<uint8_t> checkpoint(4, 0);
		constexpr uint64_t checkpointVa = 4ULL << 32;
		s.memory.map(checkpointVa, checkpoint);
		const tgm_relayout_move move = {kSurfaceVa,    kSurfaceRowsVa, kRowsVa,
						s.bytesPerRow, operation,      s.rows.size(),
						checkpointVa};
		uint32_t words[TGM_RELAYOUT_DWORDS];
		const uint32_t count = tgm_relayout_push(words, &s.l, &move, nullptr, 0);
		ASSERT_LE(count, TGM_RELAYOUT_DWORDS);
		CopyEngine ce(s.memory);
		ce.failLaunch = 2;
		ce.failAfterBytes = 23;
		ce.run(words, count);
		ASSERT_TRUE(ce.fault);
		ASSERT_EQ(checkpoint[0], 1);
		std::vector<uint64_t> surfacePages, rowPages;
		for (size_t offset = 0; offset < s.surface.size(); offset += 4096)
			surfacePages.push_back(kSurfaceVa + offset);
		for (size_t offset = 0; offset < s.rows.size(); offset += 4096)
			rowPages.push_back(kRowsVa + offset);
		const tgm_relayout_pages surface = {surfacePages.data(),
						    uint32_t(surfacePages.size())};
		const tgm_relayout_pages rows = {rowPages.data(), uint32_t(rowPages.size())};
		cpuMemory = &s.memory;
		if (operation == TGM_RELAYOUT_TILE_IN_PLACE)
			tgm_relayout_cpu(&surface, &rows, &s.l, s.bytesPerRow, 0, cpuPage);
		else
			tgm_relayout_rows_cpu(&surface, &rows, &s.l, s.bytesPerRow, cpuPage);
		EXPECT_EQ(s.surface, expected);
	}
}

TEST(Relayout, FailureBeforeCheckpointLeavesOriginalLayoutUntouched)
{
	Surface s({64, 64, 4, 64});
	s.surface = pattern(s.surface.size(), 57);
	const auto before = s.surface;
	std::vector<uint8_t> checkpoint(4, 0);
	constexpr uint64_t checkpointVa = 4ULL << 32;
	s.memory.map(checkpointVa, checkpoint);
	const tgm_relayout_move move = {
		kSurfaceVa,    kSurfaceRowsVa, kRowsVa, s.bytesPerRow, TGM_RELAYOUT_TILE_IN_PLACE,
		s.rows.size(), checkpointVa};
	uint32_t words[TGM_RELAYOUT_DWORDS];
	const uint32_t count = tgm_relayout_push(words, &s.l, &move, nullptr, 0);
	CopyEngine ce(s.memory);
	ce.failLaunch = 1;
	ce.failAfterBytes = 23;
	ce.run(words, count);
	EXPECT_TRUE(ce.fault);
	EXPECT_EQ(checkpoint[0], 0);
	EXPECT_EQ(s.surface, before);
}

TEST(Relayout, WaitsForEveryWriterFirst)
{
	constexpr uint32_t kSemaphoreRun =
		1u << 29 | 4u << 16 | 0x0010 >> 2;  // SEMAPHOREA..D on subchannel 0
	Surface s(kCases[2]);
	s.move(TGM_RELAYOUT_TO_ROWS, {{0x1000040, 7, 5}, {0x1004040, 0xfffffffe, 6}});
	uint32_t w[TGM_RELAYOUT_DWORDS];
	const tgm_relayout_wait waits[] = {{0x10, 1, 5}, {0x20, 2, 6}};
	const tgm_relayout_move m = {kSurfaceVa,           kSurfaceRowsVa, kRowsVa, s.bytesPerRow,
				     TGM_RELAYOUT_TO_ROWS, s.rows.size(),  0};
	tgm_relayout_push(w, &s.l, &m, waits, 2);
	EXPECT_EQ(w[0], kSemaphoreRun);
	EXPECT_EQ(w[5], kSemaphoreRun);
	EXPECT_EQ(w[10] >> 13 & 7, kCopySubchannel);
}

}  // namespace
