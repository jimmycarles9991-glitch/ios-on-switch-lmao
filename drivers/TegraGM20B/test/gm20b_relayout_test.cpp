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
		memory.regions[kSurfaceVa] = &surface;
		memory.regions[kSurfaceRowsVa] = &surface;
		memory.regions[kRowsVa] = &rows;
	}

	void move(tgm_relayout_op op, const std::vector<tgm_relayout_wait> &waits = {})
	{
		CopyEngine ce(memory);
		uint32_t w[TGM_RELAYOUT_DWORDS];
		const tgm_relayout_move m = {kSurfaceVa, kSurfaceRowsVa, kRowsVa, bytesPerRow,
					     op,         rows.size()};
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

TEST(Relayout, WaitsForEveryWriterFirst)
{
	Surface s(kCases[2]);
	s.move(TGM_RELAYOUT_TO_ROWS, {{0x1000040, 7}, {0x1004040, 0xfffffffe}});
	uint32_t w[TGM_RELAYOUT_DWORDS];
	const tgm_relayout_wait waits[] = {{0x10, 1}, {0x20, 2}};
	const tgm_relayout_move m = {kSurfaceVa,    kSurfaceRowsVa,       kRowsVa,
				     s.bytesPerRow, TGM_RELAYOUT_TO_ROWS, s.rows.size()};
	tgm_relayout_push(w, &s.l, &m, waits, 2);
	EXPECT_EQ(w[0], tgm_relayout_header(0, 0x0010, 4));
	EXPECT_EQ(w[5], tgm_relayout_header(0, 0x0010, 4));
	EXPECT_EQ(w[10] >> 13 & 7, TGM_RELAYOUT_SUBC_COPY);
}

}  // namespace
