// SPDX-License-Identifier: MIT
// TegraGM20BViews.cpp on the host, over gm20b_tiled_model.h: a CPU lock of a block-linear surface gives the locking task its
// texels as rows in a scratch mapped at its own address, while the surface's pages (what the GPU and the display read)
// never change; the last unlock after a write puts the CPU's rows back into the texels; readers in several tasks share
// one scratch; nested locks map once; a task that died holding a lock is dropped; the surface stays block-linear while
// any lock is held, whatever its GPU imports do.
#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BLayout.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.h"
#include "drivers/TegraGM20B/TegraGM20BViews.h"
#include "gm20b_tiled_model.h"

namespace
{
constexpr uint32_t kLock = 2, kUnlock = 3, kReadOnly = 1, kAvoidSync = 2;
constexpr uint32_t kW = 64, kH = 64, kBpp = 4, kRow = 320, kPages = 5;

struct Tiled {
	std::vector<uint64_t> pas;
	tgm_relayout_pages pages{};
	tgm_coh *coh = nullptr;
	uint32_t id;
	std::vector<uint8_t> rows;
	tgm_layout l{};

	Tiled(uint64_t first, uint32_t surfaceId) : id(surfaceId)
	{
		for (uint32_t i = 0; i < kPages; i++)
			pas.push_back(first + i * 0x2000);
		pages = {pas.data(), kPages};
		rows.resize(kPages * 4096);
		for (size_t i = 0; i < rows.size(); i++)
			rows[i] = uint8_t(i * 31 + surfaceId);
		scatter(pages, rows);
		int covered = 0;
		coh = tgm_coherency_add(pas[0], id, pas.data(), kPages, 1, 1, &covered);
		EXPECT_EQ(tgm_coherency_pin(coh, reinterpret_cast<switch_gpu_backing *>(1), nullptr,
					    nullptr),
			  0);
		EXPECT_EQ(tgm_coherency_layout(coh, kW, kH, kBpp, kRow, 1), 1);
		EXPECT_TRUE(tgm_layout_tiled(kW, kH, kBpp, &l));
	}

	std::vector<uint8_t> texels() const
	{
		return gather(pages);
	}

	// what a task reading its mapping at `address` sees as rows
	std::vector<uint8_t> seen(void *task, uint64_t address) const
	{
		void *m = gViews.mapped.count({task, address}) ? gViews.mapped.at({task, address})
							       : ViewModel::surface(id);
		if (m == ViewModel::surface(id))
			return gather(pages);
		const auto *s = static_cast<const tgm_scratch *>(m);
		return gather({s->pas.data(), uint32_t(s->pas.size())});
	}

	bool rowsAre(const std::vector<uint8_t> &got, const std::vector<uint8_t> &want) const
	{
		for (uint32_t y = 0; y < kH; y++)
			if (memcmp(&got[y * kRow], &want[y * kRow], kW * kBpp))
				return false;
		return true;
	}
};

void *task(int n)
{
	return reinterpret_cast<void *>(uintptr_t(0x1000 * n));
}

void *client(int n)
{
	return reinterpret_cast<void *>(uintptr_t(0x1000 * n + 8));
}

void map(int n, uint32_t id, uint64_t address)
{
	gViews.mappings[{client(n), id}] = {client(n), task(n), address, kPages * 4096, 0, 0};
}

class Views : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_EQ(tgm_coherency_init(), 0);
		gMoves.clear();
		gMoveFails = false;
		gCpuMoves = 0;
		gScratchFails = false;
		gScratchPurged = false;
		gRefusals.clear();
		gViews = ViewModel{};
		gGpu = FakeGpu{};
	}
};

TEST_F(Views, ALockSeesRowsWhileTheGpuAndDisplayKeepTexels)
{
	Tiled s(0xa0000000, 81);
	map(1, s.id, 0x10000);
	const int put = gViews.scratchesPut;  // (tiling's own)
	const std::vector<uint8_t> texels = s.texels();
	gMoves.clear();
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	ASSERT_EQ(gMoves.size(), 1u);
	EXPECT_EQ(gMoves[0].op, TGM_RELAYOUT_TO_ROWS);
	EXPECT_TRUE(s.rowsAre(s.seen(task(1), 0x10000), s.rows));
	EXPECT_EQ(s.texels(), texels);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(s.pas[0]));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	EXPECT_EQ(gMoves.size(), 1u);  // read-only: nothing to put back
	EXPECT_EQ((gViews.mapped[{task(1), 0x10000}]), ViewModel::surface(s.id));
	EXPECT_EQ(s.texels(), texels);
	EXPECT_EQ(gViews.scratchesPut - put, 1);
	tgm_coherency_put(s.coh);
}

// F-v1, gpu.path.resource_ready: a render into the surface while a CPU write lock holds its rows. On Apple's GPUs the
// render runs and its pixels stay where the CPU wrote none; here the unlock lays only the CPU's changes over them.
TEST_F(Views, AGpuWriteUnderAWriteLockKeepsItsBytesWhereTheCpuWroteNone)
{
	Tiled s(0xa4000000, 85);
	map(1, s.id, 0x10000);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	const size_t lockMoves = gMoves.size();
	tgm_views_gpu_writes(s.coh);
	ASSERT_EQ(gMoves.size(), lockMoves + 1);
	EXPECT_EQ(gMoves.back().op, TGM_RELAYOUT_TO_ROWS);
	tgm_views_gpu_writes(s.coh);  // a second writer keeps the first one's base
	EXPECT_EQ(gMoves.size(), lockMoves + 1);

	std::vector<uint8_t> green(s.rows.size(), 0x5a), texels = s.texels();
	tgm_layout_write(&s.l, texels.data(), 0, 0, kW, kH, green.data(), kRow);
	scatter(s.pages, texels);
	auto *scratch = static_cast<const tgm_scratch *>(gViews.mapped.at({task(1), 0x10000}));
	const tgm_relayout_pages viewPages = {scratch->pas.data(), uint32_t(scratch->pas.size())};
	std::vector<uint8_t> view = gather(viewPages);
	memset(&view[3 * kRow], 0xc3, kW * kBpp);  // the CPU writes row 3 only
	scatter(viewPages, view);

	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, 0), 0);
	std::vector<uint8_t> back(s.rows.size());
	const std::vector<uint8_t> after = s.texels();
	tgm_layout_read(&s.l, after.data(), 0, 0, kW, kH, back.data(), kRow);
	// a byte the CPU wrote with the value its rows already held is not told from one it left alone
	std::vector<uint8_t> want(green);
	for (uint32_t x = 0; x < kW * kBpp; x++)
		want[3 * kRow + x] = s.rows[3 * kRow + x] == 0xc3 ? 0x5a : 0xc3;
	EXPECT_TRUE(s.rowsAre(back, want));
	tgm_coherency_put(s.coh);
}

// gpu.path.resource_ready on SubmitJob, raw's path since 57b71739d. A job's written list as runJobUses builds it: the
// coherency entry of each written shared use, none for a BO without one. The job's entry keeps a write-locked surface's
// rows once, keeps nothing for a read-locked one, and the render survives the unlock where the CPU wrote none. Without the
// call, as on 8ac4f2e41's SubmitJob, the unlock lays the lock's rows over the render.
TEST_F(Views, AJobsWrittenListKeepsAWriteLockedRenderAcrossTheUnlock)
{
	for (const bool called : {true, false}) {
		Tiled target(called ? 0xa8000000 : 0xaa000000, called ? 87 : 88);
		Tiled sampled(called ? 0xac000000 : 0xae000000, called ? 89 : 90);
		map(1, target.id, 0x10000);
		map(1, sampled.id, 0x20000);
		ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, target.id, 0), 0);
		ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, sampled.id, kReadOnly), 0);
		const size_t locked = gMoves.size();
		tgm_coh *const written[] = {nullptr, target.coh, sampled.coh, target.coh};
		if (called)
			tgm_views_job_writes(written, 4);
		ASSERT_EQ(gMoves.size(), locked + (called ? 1u : 0u));
		if (called)
			EXPECT_EQ(gMoves.back().op, TGM_RELAYOUT_TO_ROWS);

		std::vector<uint8_t> green(target.rows.size(), 0x5a), texels = target.texels();
		tgm_layout_write(&target.l, texels.data(), 0, 0, kW, kH, green.data(), kRow);
		scatter(target.pages, texels);
		ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, target.id, 0), 0);
		ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, sampled.id, kReadOnly), 0);
		std::vector<uint8_t> back(target.rows.size());
		const std::vector<uint8_t> after = target.texels();
		tgm_layout_read(&target.l, after.data(), 0, 0, kW, kH, back.data(), kRow);
		EXPECT_EQ(target.rowsAre(back, green), called);
		EXPECT_EQ(target.rowsAre(back, target.rows), !called);
		tgm_coherency_put(target.coh);
		tgm_coherency_put(sampled.coh);
	}
}

TEST_F(Views, ReadOnlyLocksAndUnlockedSurfacesKeepNoBase)
{
	Tiled s(0xa6000000, 86);
	map(1, s.id, 0x10000);
	const size_t before = gMoves.size();
	tgm_views_gpu_writes(s.coh);
	tgm_views_gpu_writes(nullptr);
	EXPECT_EQ(gMoves.size(), before);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	const size_t locked = gMoves.size();
	tgm_views_gpu_writes(s.coh);
	EXPECT_EQ(gMoves.size(), locked);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	tgm_coherency_put(s.coh);
}

// The copy engine failing (a faulted copy, no channel or mappings) never leaves a lock with texels or a write unlanded:
// the CPU makes the same moves through the pages, clearing the scratch's stale bytes first
TEST_F(Views, MovesTheCopyEngineCannotMakeAreTheCpus)
{
	Tiled s(0xa0700000, 87);
	map(1, s.id, 0x80000);
	const std::vector<uint8_t> texels = s.texels();
	gMoveFails = true;
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	EXPECT_EQ(gCpuMoves, 1);
	ASSERT_EQ(gRefusals.size(), 1u);
	EXPECT_EQ(gRefusals[0],
		  std::make_pair(std::string("iosurface-layout"), std::string("ce-move-failed")));
	EXPECT_TRUE(s.rowsAre(s.seen(task(1), 0x80000), s.rows));
	const std::vector<uint8_t> seen = s.seen(task(1), 0x80000);
	for (uint32_t y = 0; y < kH; y++)
		EXPECT_TRUE(std::all_of(seen.begin() + y * kRow + kW * kBpp,
					seen.begin() + (y + 1) * kRow, [](uint8_t b) {
						return b == 0;
					}));
	EXPECT_EQ(s.texels(), texels);
	const auto *scratch = static_cast<const tgm_scratch *>(gViews.mapped[{task(1), 0x80000}]);
	const tgm_relayout_pages rows = {scratch->pas.data(), uint32_t(scratch->pas.size())};
	std::vector<uint8_t> written = gather(rows);
	for (uint32_t y = 0; y < kH; y++)
		memset(&written[y * kRow], int(y + 7), kW * kBpp);
	scatter(rows, written);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, 0), 0);
	EXPECT_EQ(gCpuMoves, 2);
	EXPECT_EQ(gRefusals.size(), 2u);
	std::vector<uint8_t> back(kRow * kH);
	const std::vector<uint8_t> after = s.texels();
	tgm_layout_read(&s.l, after.data(), 0, 0, kW, kH, back.data(), kRow);
	EXPECT_TRUE(s.rowsAre(back, written));
	gMoveFails = false;
	tgm_coherency_put(s.coh);
}

// The CPU's move reads and writes DRAM behind the GPU's L2: GPU writes that completed without reaching DRAM (an Internal
// job's, no writer the registry still lists) get there first, at the dead task's drain as at a lock. The copy engine
// reads through the L2, so its moves wait for nothing.
TEST_F(Views, ACpuMoveWaitsForCompletedGpuWritesToReachDram)
{
	Tiled s(0xa0900000, 88);
	map(1, s.id, 0x80000);
	map(2, s.id, 0x90000);
	gGpu.l2Dirty = true;
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	EXPECT_EQ(gGpu.visibleWaits, 0);
	gMoveFails = true;
	gViews.dead.insert(client(1));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(2), kLock, s.id, kReadOnly), 0);
	EXPECT_EQ(gCpuMoves, 2);
	EXPECT_EQ(gGpu.staleCpuMoves, 0);
	EXPECT_GE(gGpu.visibleWaits, 1);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(2), kUnlock, s.id, kReadOnly), 0);
	gMoveFails = false;
	tgm_coherency_put(s.coh);
}

// No memory for the rows: the lock is refused with nothing mapped or moved, never handed the texels
TEST_F(Views, ALockWithNoScratchIsRefused)
{
	Tiled s(0xa0800000, 88);
	map(1, s.id, 0x90000);
	gMoves.clear();
	gScratchFails = true;
	EXPECT_NE(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(gViews.remaps, 0);
	void *rows = nullptr, *memory = nullptr;
	uint64_t offset = 0;
	EXPECT_EQ(tegra_gm20b_surface_rows(s.pas[1] + 16, &rows, &memory, &offset), -12);
	const auto noScratch =
		std::make_pair(std::string("iosurface-layout"), std::string("no-scratch"));
	EXPECT_EQ(gRefusals, decltype(gRefusals)({noScratch, noScratch}));
	gScratchFails = false;
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	EXPECT_TRUE(s.rowsAre(s.seen(task(1), 0x90000), s.rows));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, 0), 0);
	tgm_coherency_put(s.coh);
}

// A lock that could not show its task rows is refused before anything is held, and says why
TEST_F(Views, LocksThatCouldShowTexelsAreRefused)
{
	Tiled s(0xa0900000, 89);
	map(1, s.id, 0xa0000);
	map(2, s.id, 0xb0000);
	const int put = gViews.scratchesPut;
	gMoves.clear();
	gViews.badMappings.insert(client(1));
	EXPECT_NE(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	gViews.badMappings.clear();
	gViews.remapFails = 1;
	EXPECT_NE(tegra_gm20b_iosurface_view(client(2), kLock, s.id, 0), 0);
	EXPECT_EQ(gViews.scratchesPut - put, 1);  // the second's fill, given back
	EXPECT_TRUE(gViews.mapped.empty());
	const auto refused = [](const char *why) {
		return std::make_pair(std::string("iosurface-layout"), std::string(why));
	};
	EXPECT_EQ(gRefusals,
		  decltype(gRefusals)({refused("partial-map"), refused("remap-failed")}));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(2), kLock, s.id, kReadOnly),
		  0);  // and nothing was left held
	EXPECT_TRUE(s.rowsAre(s.seen(task(2), 0xb0000), s.rows));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(2), kUnlock, s.id, kReadOnly), 0);
	EXPECT_EQ(gViews.scratchesDiscarded, 0);
	tgm_coherency_put(s.coh);
}

// A mapping that cannot be given its surface back keeps the scratch, so that scratch is never handed to another surface
TEST_F(Views, AScratchATaskStillMapsIsNeverReused)
{
	Tiled s(0xa0a00000, 90);
	map(1, s.id, 0xc0000);
	gViews.restoreFails.insert(client(1));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	const int put = gViews.scratchesPut;
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	EXPECT_EQ(gViews.scratchesPut, put);
	EXPECT_EQ(gViews.scratchesDiscarded, 1);
	ASSERT_EQ(gRefusals.size(), 1u);
	EXPECT_EQ(gRefusals[0].second, "restore-failed");
	tgm_coherency_put(s.coh);
}

TEST_F(Views, AWriteLandsInTheTexelsAtTheLastUnlock)
{
	Tiled s(0xa0100000, 82);
	map(1, s.id, 0x20000);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	const auto *scratch = static_cast<const tgm_scratch *>(gViews.mapped[{task(1), 0x20000}]);
	const tgm_relayout_pages rows = {scratch->pas.data(), uint32_t(scratch->pas.size())};
	std::vector<uint8_t> written = gather(rows);
	for (uint32_t y = 0; y < kH; y++)
		memset(&written[y * kRow], int(y), kW * kBpp);
	scatter(rows, written);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, 0), 0);
	ASSERT_EQ(gMoves.back().op, TGM_RELAYOUT_TO_TILED);
	std::vector<uint8_t> back(kRow * kH);
	const std::vector<uint8_t> texels = s.texels();
	tgm_layout_read(&s.l, texels.data(), 0, 0, kW, kH, back.data(), kRow);
	EXPECT_TRUE(s.rowsAre(back, written));
	tgm_coherency_put(s.coh);
}

TEST_F(Views, ReadersInSeveralTasksShareOneScratchAndAWriterAmongThem)
{
	Tiled s(0xa0200000, 83);
	const int put = gViews.scratchesPut;
	map(1, s.id, 0x30000);
	map(2, s.id, 0x40000);
	map(3, s.id, 0x50000);
	gMoves.clear();
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(2), kLock, s.id, kReadOnly), 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(3), kLock, s.id, 0), 0);
	EXPECT_EQ(gMoves.size(), 1u);
	EXPECT_EQ((gViews.mapped[{task(1), 0x30000}]), (gViews.mapped[{task(2), 0x40000}]));
	EXPECT_EQ((gViews.mapped[{task(2), 0x40000}]), (gViews.mapped[{task(3), 0x50000}]));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(3), kUnlock, s.id, 0), 0);
	EXPECT_EQ(gMoves.size(),
		  1u);  // others still read the scratch: the rows go back at the last unlock
	EXPECT_EQ((gViews.mapped[{task(3), 0x50000}]), ViewModel::surface(s.id));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(2), kUnlock, s.id, kReadOnly), 0);
	ASSERT_EQ(gMoves.size(), 2u);
	EXPECT_EQ(gMoves[1].op, TGM_RELAYOUT_TO_TILED);
	EXPECT_EQ(gViews.scratchesPut - put, 1);
	tgm_coherency_put(s.coh);
}

// iconservicesagent reads each icon render back with three read-only locks in a row: only the first moves texels; a
// GPU submission that may write the surface, a write lock or a purged scratch makes the next lock move them again
TEST_F(Views, ReadOnlyLocksAfterTheFirstReuseItsRowsUntilTheGpuWrites)
{
	Tiled s(0xa0700000, 88);
	map(1, s.id, 0x90000);
	gMoves.clear();
	const int reclaimed = gScratchesReclaimed;
	for (int i = 0; i < 3; i++) {
		ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
		EXPECT_TRUE(s.rowsAre(s.seen(task(1), 0x90000), s.rows)) << i;
		ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	}
	EXPECT_EQ(gMoves.size(), 1u);
	EXPECT_EQ(gScratchesReclaimed - reclaimed, 2);
	tgm_coherency_wrote(s.coh, 1, 7, 1);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	EXPECT_EQ(gMoves.size(), 2u);
	gBackingContent++;  // VIC or a CPU writer elsewhere, recorded on the backing
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	EXPECT_EQ(gMoves.size(), 3u);
	gScratchPurged = true;
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	EXPECT_EQ(gMoves.size(), 4u);
	gScratchPurged = false;
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, 0), 0);
	EXPECT_EQ(gMoves.size(), 5u);  // reused rows, then the write back to the texels
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	EXPECT_EQ(gMoves.size(), 6u);
	EXPECT_EQ(gMoves.back().op, TGM_RELAYOUT_TO_ROWS);
	tgm_coherency_put(s.coh);
}

TEST_F(Views, NestedLocksInOneTaskMapOnce)
{
	Tiled s(0xa0300000, 84);
	map(1, s.id, 0x60000);
	gViews.remaps = 0;
	for (int i = 0; i < 3; i++)
		ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	EXPECT_EQ(gViews.remaps, 1);
	for (int i = 0; i < 3; i++)
		ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	EXPECT_EQ(gViews.remaps, 2);
	EXPECT_EQ((gViews.mapped[{task(1), 0x60000}]), ViewModel::surface(s.id));
	EXPECT_EQ(gViews.holds[client(1)], 0);
	tgm_coherency_put(s.coh);
}

TEST_F(Views, AvoidSyncWithAWriterRunningIsRefusedAndMovesNothing)
{
	Tiled s(0xa0400000, 85);
	map(1, s.id, 0x70000);
	gGpu.completed[9] = 2;
	tgm_coherency_wrote(s.coh, 0, 9, 3);  // channel 9 has not completed seqno 3
	gMoves.clear();
	EXPECT_NE(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kAvoidSync), 0);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(gViews.remaps, 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	ASSERT_EQ(gMoves.size(), 1u);
	ASSERT_EQ(gMoves[0].writers.size(), 1u);  // behind the writer on the GPU
	EXPECT_EQ(gMoves[0].writers[0].uid, 9u);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, 0), 0);
	tgm_coherency_put(s.coh);
}

TEST_F(Views, ATaskThatDiedHoldingALockIsDropped)
{
	Tiled s(0xa0500000, 86);
	map(1, s.id, 0x80000);
	map(2, s.id, 0x90000);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, 0), 0);
	gViews.dead.insert(client(1));
	gMoves.clear();
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(2), kLock, s.id, kReadOnly), 0);
	// the dead task's write lock ended with it: its rows went back, and the new lock filled a scratch afresh
	ASSERT_EQ(gMoves.size(), 2u);
	EXPECT_EQ(gMoves[0].op, TGM_RELAYOUT_TO_TILED);
	EXPECT_EQ(gMoves[1].op, TGM_RELAYOUT_TO_ROWS);
	EXPECT_EQ(gViews.holds[client(1)], 0);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(2), kUnlock, s.id, kReadOnly), 0);
	tgm_coherency_put(s.coh);
}

TEST_F(Views, ALockKeepsTheSurfaceTiledWhenItsGpuImportGoes)
{
	Tiled s(0xa0600000, 87);
	map(1, s.id, 0xa0000);
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, s.id, kReadOnly), 0);
	gMoves.clear();
	tgm_coherency_put(s.coh);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_TRUE(tegra_gm20b_surface_tiled(s.pas[0]));
	ASSERT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, s.id, kReadOnly), 0);
	ASSERT_EQ(gMoves.size(), 1u);
	EXPECT_EQ(gMoves[0].op, TGM_RELAYOUT_UNTILE_IN_PLACE);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(s.pas[0]));
	EXPECT_TRUE(s.rowsAre(s.texels(), s.rows));
}

TEST_F(Views, NothingForAPitchSurface)
{
	std::vector<uint64_t> pas = {0xa0700000};
	int covered = 0;
	tgm_coh *c = tgm_coherency_add(pas[0], 88, pas.data(), 1, 1, 1, &covered);
	map(1, 88, 0xb0000);
	EXPECT_EQ(tegra_gm20b_iosurface_view(client(1), kLock, 88, 0), 0);
	EXPECT_EQ(tegra_gm20b_iosurface_view(client(1), kUnlock, 88, 0), 0);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(gViews.remaps, 0);
	tgm_coherency_put(c);
}

}  // namespace
