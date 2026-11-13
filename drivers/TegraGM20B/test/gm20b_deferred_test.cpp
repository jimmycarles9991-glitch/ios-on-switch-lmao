// SPDX-License-Identifier: MIT
// The relayouts a surface's history makes unnecessary (TegraGM20BTiled.cpp decide, TegraGM20BCoherency.cpp drop, and
// TegraGM20BViews.cpp's first lock), over gm20b_tiled_model.h: a first decision over memory nothing wrote moves nothing,
// and any write before it (the surface's seed, a submission, the backing's record) brings the move back; a last import
// leaves a block-linear surface on its memory's record, whose first CPU lock, next import or other engine moves its
// texels back to rows, and whose memory going ends it unmoved.
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.h"
#include "drivers/TegraGM20B/TegraGM20BTiled.h"
#include "drivers/TegraGM20B/TegraGM20BViews.h"
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"
#include "gm20b_record_model.h"
#include "gm20b_tiled_model.h"

namespace
{
constexpr uint32_t kW = 64, kH = 64, kBpp = 4, kRow = 320, kPages = 5, kLock = 2, kUnlock = 3,
		   kReadOnly = 1;

int gSurface;
int gReleases;

void releaseSurface(void *surface)
{
	EXPECT_EQ(surface, &gSurface);
	gReleases++;
}

std::vector<uint64_t> pages(uint64_t first)
{
	std::vector<uint64_t> v;
	for (uint32_t i = 0; i < kPages; i++)
		v.push_back(first + i * 0x2000);
	return v;
}

tgm_coh *import(const std::vector<uint64_t> &p, uint32_t id, tgm_record &record, int fresh)
{
	int covered = 0;
	tgm_coh *c = tgm_coherency_add(p[0], id, p.data(), kPages, 1, 1, &covered);
	EXPECT_EQ(tgm_coherency_pin(c, reinterpret_cast<switch_gpu_backing *>(1), &gSurface,
				    releaseSurface),
		  0);
	tgm_coherency_import(c, &record, fresh);
	return c;
}

int ask(tgm_coh *c)
{
	tgm_coh_hold_asker(c);
	const int layout = tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1);
	tgm_coh_put_asker(c);
	return layout;
}

bool gone(uint64_t pa)
{
	tgm_coh *c = tgm_coh_find_pa(pa);
	if (c)
		tgm_coherency_put(c);
	return c == nullptr;
}

int movesOf(int op)
{
	int n = 0;
	for (const Move &m : gMoves)
		n += static_cast<int>(m.op == op);
	return n;
}

class Deferred : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_EQ(tgm_coherency_init(), 0);
		gMoves.clear();
		gMoveFails = false;
		gScratchFails = false;
		gRefusals.clear();
		gGpu = FakeGpu{};
		gBackingContent = 0;
		gSurfaceUnwritten = true;
		gReleases = 0;
	}
};

TEST_F(Deferred, AFirstDecisionOverMemoryNothingWroteMovesNothing)
{
	const auto p = pages(0xd0000000);
	tgm_record record{nullptr, 301};
	tgm_coh *c = import(p, 301, record, 1);
	EXPECT_EQ(ask(c), 1);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_TRUE(gMoves.empty());
	tgm_coherency_put(c);
	tgm_coherency_record_gone(&record);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_TRUE(gone(p[0]));
}

TEST_F(Deferred, AnyWriteBeforeTheDecisionBringsTheTilingMoveBack)
{
	struct Case {
		const char *what;
		int fresh;
		bool seedMoved, submitted;
		uint64_t backingWrites;
	};

	const Case cases[] = {
		{"an import of memory a GPU import had before", 0, false, false, 0},
		{"the surface's seed moved", 1, true, false, 0},
		{"a submission wrote it", 1, false, true, 0},
		{"its backing recorded a write", 1, false, false, 1},
	};
	uint64_t first = 0xd0100000;
	for (const Case &k : cases) {
		gMoves.clear();
		gSurfaceUnwritten = !k.seedMoved;
		gBackingContent = k.backingWrites;
		const auto p = pages(first += 0x100000);
		tgm_record record{nullptr, 302};
		tgm_coh *c = import(p, 302, record, k.fresh);
		if (k.submitted)
			tgm_coherency_wrote(c, 1, 9, 1);
		gGpu.completed[9] = 1;
		EXPECT_EQ(ask(c), 1) << k.what;
		EXPECT_EQ(movesOf(TGM_RELAYOUT_TILE_IN_PLACE), 1) << k.what;
		tgm_coherency_put(c);
		tgm_coherency_record_gone(&record);
		EXPECT_TRUE(gone(p[0])) << k.what;
	}
}

TEST_F(Deferred, ALastImportLeavesTheSurfaceBlockLinearOnItsRecord)
{
	const auto p = pages(0xd1000000);
	tgm_record record{nullptr, 303};
	tgm_coh *c = import(p, 303, record, 1);
	EXPECT_EQ(ask(c), 1);
	tgm_coherency_put(c);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(record.deferred, c);
	EXPECT_EQ(gReleases, 1);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	tgm_coherency_record_gone(&record);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(record.deferred, nullptr);
	EXPECT_TRUE(gone(p[0]));
	EXPECT_EQ(gReleases, 1);
}

TEST_F(Deferred, TheFirstCpuLockMovesTheTexelsBackOnceAndTheSurfaceIsPitch)
{
	const auto p = pages(0xd1100000);
	tgm_record record{nullptr, 304};
	tgm_coh *c = import(p, 304, record, 0);
	EXPECT_EQ(ask(c), 1);
	tgm_coherency_put(c);
	ASSERT_EQ(movesOf(TGM_RELAYOUT_TILE_IN_PLACE), 1);
	ASSERT_EQ(record.deferred, c);
	EXPECT_EQ(tgm_views_trap(reinterpret_cast<void *>(0x10), kLock, 304, kReadOnly), 0);
	EXPECT_EQ(movesOf(TGM_RELAYOUT_UNTILE_IN_PLACE), 1);
	EXPECT_EQ(record.deferred, nullptr);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_TRUE(gone(p[0]));
	EXPECT_EQ(tgm_views_trap(reinterpret_cast<void *>(0x10), kUnlock, 304, kReadOnly), 0);
	EXPECT_EQ(gMoves.size(), 2u);
}

// BoImport untiles a deferred surface before its import (an importer that never asks binds rows); while the display
// scans it the import takes it back block-linear, and the release after the display's defers it again
TEST_F(Deferred, ANewImportGetsRowsUnlessTheDisplayHoldsIt)
{
	const auto p = pages(0xd1200000);
	tgm_record record{nullptr, 305};
	tgm_coh *c = import(p, 305, record, 1);
	EXPECT_EQ(ask(c), 1);
	tgm_coherency_put(c);
	ASSERT_EQ(record.deferred, c);
	int tiled = 0;
	void *hold = tegra_gm20b_surface_hold(p[0], &tiled);
	tgm_coherency_rows_at(p[0]);
	EXPECT_TRUE(gMoves.empty());
	tgm_coh *again = import(p, 305, record, 0);
	EXPECT_EQ(again, c);
	EXPECT_EQ(record.deferred, nullptr);
	EXPECT_EQ(tgm_coherency_layout(again, kW, kH, kBpp, kRow, 0), 1);
	tgm_coherency_put(again);
	EXPECT_EQ(record.deferred, nullptr);
	tegra_gm20b_surface_unhold(hold);
	EXPECT_EQ(record.deferred, c);
	EXPECT_EQ(gReleases, 2);
	tgm_coherency_rows_at(p[0]);
	EXPECT_EQ(movesOf(TGM_RELAYOUT_UNTILE_IN_PLACE), 1);
	EXPECT_EQ(record.deferred, nullptr);
	EXPECT_TRUE(gone(p[0]));
}

TEST_F(Deferred, ALockOfASurfaceThatTookTheIdAgainLeavesTheDeferredEntryAlone)
{
	const auto p = pages(0xd1300000);
	tgm_record record{nullptr, 999};
	tgm_coh *c = import(p, 306, record, 1);
	EXPECT_EQ(ask(c), 1);
	tgm_coherency_put(c);
	ASSERT_EQ(record.deferred, c);
	EXPECT_EQ(tgm_views_trap(reinterpret_cast<void *>(0x10), kLock, 306, kReadOnly), 0);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(record.deferred, c);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	tgm_coherency_record_gone(&record);
	EXPECT_TRUE(gone(p[0]));
}

// The display scans its texels: the lock reads them through a view, and the surface stays block-linear on its record
TEST_F(Deferred, ALockWhileTheDisplayHoldsTheSurfaceReadsItThroughAView)
{
	const auto p = pages(0xd1400000);
	tgm_record record{nullptr, 307};
	tgm_coh *c = import(p, 307, record, 1);
	EXPECT_EQ(ask(c), 1);
	tgm_coherency_put(c);
	int tiled = 0;
	void *hold = tegra_gm20b_surface_hold(p[0], &tiled);
	ASSERT_EQ(tiled, 1);
	void *client = reinterpret_cast<void *>(0x20);
	EXPECT_EQ(tgm_views_trap(client, kLock, 307, kReadOnly), 0);
	EXPECT_EQ(movesOf(TGM_RELAYOUT_UNTILE_IN_PLACE), 0);
	EXPECT_EQ(movesOf(TGM_RELAYOUT_TO_ROWS), 1);
	EXPECT_EQ(tgm_views_trap(client, kUnlock, 307, kReadOnly), 0);
	tegra_gm20b_surface_unhold(hold);
	EXPECT_EQ(record.deferred, c);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	tgm_coherency_record_gone(&record);
	EXPECT_TRUE(gone(p[0]));
}

// VIC, NVJPG and NVENC take a surface's pages as rows (TegraVIC's surface acquisition): a deferred one is untiled first,
// unless the display scans it, and a surface no import ever deferred is left alone
TEST_F(Deferred, AnEngineTakingTheSurfaceGetsRowsUnlessTheDisplayScansIt)
{
	const auto p = pages(0xd1500000);
	tgm_record record{nullptr, 308};
	tgm_coh *c = import(p, 308, record, 1);
	EXPECT_EQ(ask(c), 1);
	tgm_coherency_put(c);
	int tiled = 0;
	void *hold = tegra_gm20b_surface_hold(p[0], &tiled);
	tgm_coherency_engine_rows(&record);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(record.deferred, c);
	tegra_gm20b_surface_unhold(hold);
	tgm_coherency_engine_rows(&record);
	EXPECT_EQ(movesOf(TGM_RELAYOUT_UNTILE_IN_PLACE), 1);
	EXPECT_EQ(record.deferred, nullptr);
	EXPECT_TRUE(gone(p[0]));
	tgm_coherency_engine_rows(&record);
	EXPECT_EQ(gMoves.size(), 1u);
}

// A kIOSurfaceLockAvoidSync lock while the GPU still writes the surface is refused, as for an imported one
TEST_F(Deferred, AnAvoidSyncLockBehindRunningWritersIsRefusedAndMovesNothing)
{
	const auto p = pages(0xd1600000);
	tgm_record record{nullptr, 309};
	tgm_coh *c = import(p, 309, record, 1);
	EXPECT_EQ(ask(c), 1);
	tgm_coherency_wrote(c, 1, 11, 5);
	gGpu.completed[11] = 4;
	tgm_coherency_put(c);
	ASSERT_EQ(record.deferred, c);
	EXPECT_NE(tgm_views_trap(reinterpret_cast<void *>(0x30), kLock, 309, kReadOnly | 2), 0);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(record.deferred, c);
	gGpu.completed[11] = 5;
	tgm_coherency_record_gone(&record);
	EXPECT_TRUE(gone(p[0]));
}
}  // namespace
