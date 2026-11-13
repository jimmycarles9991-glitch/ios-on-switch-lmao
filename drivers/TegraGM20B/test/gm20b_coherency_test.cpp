// SPDX-License-Identifier: MIT
// TegraGM20BCoherency.cpp on the host: which IOSurface lock/unlock traps clean or invalidate an import, with how many
// pages, across imports of the same surface and their release.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BTiled.h"
#include "drivers/TegraGM20B/TegraGM20BLayout.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.h"
#include "drivers/TegraGM20B/TegraGM20BTiled.h"
#include "drivers/TegraGM20B/TegraGM20BViews.h"
#include "gm20b_tiled_model.h"
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"

std::vector<std::vector<uint64_t>> &cleanCalls()
{
	static std::vector<std::vector<uint64_t>> calls;
	return calls;
}

static std::mutex gCleanLock;

// The kernel's process name and ID, for the debug traces of CPU locks and unlocks.
extern "C" void proc_selfname(char *buf, int size)
{
	snprintf(buf, size_t(size), "coherency_test");
}

extern "C" int proc_selfpid(void)
{
	return 1;
}

std::set<uint64_t> gCpuLines;

extern "C" void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages)
{
	std::lock_guard<std::mutex> hold(gCleanLock);
	cleanCalls().emplace_back(pas, pas + npages);
	for (uint32_t i = 0; i < npages; i++)
		gCpuLines.erase(pas[i]);
}

FakeGpu gGpu;

extern "C" int tgm_writer_done(uint32_t uid, uint32_t seqno)
{
	const auto it = gGpu.completed.find(uid);
	return gGpu.killed.count(uid) || it == gGpu.completed.end() ||
	       int32_t(it->second - seqno) >= 0;
}

extern "C" void tgm_writer_wait(uint32_t uid, uint32_t seqno)
{
	gGpu.waits.emplace_back(uid, seqno);
	gGpu.completed[uid] = seqno;
}

extern "C" int switch_gpu_backing_wait_visible(switch_gpu_backing *)
{
	gGpu.visibleWaits++;
	gGpu.l2Dirty = false;
	return 0;
}

extern "C" int tgm_writers_all_done(void)
{
	return gGpu.allDone;
}

std::map<uint64_t, std::vector<uint8_t>> gPages;

std::vector<uint8_t> gather(const tgm_relayout_pages &p)
{
	std::vector<uint8_t> out;
	for (uint32_t i = 0; i < p.npages; i++) {
		auto &page = gPages[p.pas[i]];
		page.resize(4096);
		out.insert(out.end(), page.begin(), page.end());
	}
	return out;
}

void scatter(const tgm_relayout_pages &p, const std::vector<uint8_t> &bytes)
{
	for (uint32_t i = 0; i < p.npages; i++)
		gPages[p.pas[i]].assign(bytes.begin() + i * 4096, bytes.begin() + (i + 1) * 4096);
}

std::vector<Move> gMoves;
bool gMoveFails;
int gCpuMoves;
bool gScratchFails;
std::vector<std::pair<std::string, std::string>> gRefusals;

extern "C" void switch_refusal(const char *domain, const char *reason, const char *, ...)
{
	gRefusals.emplace_back(domain, reason);
}

ViewModel gViews;

extern "C" int tgm_relayout_init(void)
{
	return 0;
}

extern "C" int tgm_relayout(const struct tgm_relayout_pages *surface,
			    const struct tgm_relayout_pages *rows, const struct tgm_layout *l,
			    uint32_t bytes_per_row, int op,
			    const struct tgm_relayout_writer *writers, uint32_t nwriters)
{
	bool rowsClean = true;
	{
		std::lock_guard<std::mutex> hold(gCleanLock);
		for (uint32_t i = 0; i < rows->npages; i++)
			rowsClean = rowsClean && !gCpuLines.count(rows->pas[i]);
	}
	gMoves.push_back(
		{op, std::vector<tgm_relayout_writer>(writers, writers + nwriters), rowsClean});
	if (gMoveFails)
		return -5;
	std::vector<uint8_t> s = gather(*surface), r = gather(*rows);
	if (op == TGM_RELAYOUT_TILE_IN_PLACE)
		r.assign(s.begin(), s.begin() + r.size());
	if (op == TGM_RELAYOUT_TO_ROWS || op == TGM_RELAYOUT_UNTILE_IN_PLACE)
		tgm_layout_read(l, s.data(), 0, 0, l->width, l->height, r.data(), bytes_per_row);
	else
		tgm_layout_write(l, s.data(), 0, 0, l->width, l->height, r.data(), bytes_per_row);
	if (op == TGM_RELAYOUT_UNTILE_IN_PLACE)
		std::copy(r.begin(), r.end(), s.begin());
	scatter(*surface, s);
	scatter(*rows, r);
	return 0;
}

namespace
{
uint8_t *modelPage(uint64_t pa)
{
	auto &page = gPages[pa];
	page.resize(4096);
	return page.data();
}
}  // namespace

extern "C" void tgm_relayout_by_cpu(const struct tgm_relayout_pages *surface,
				    const struct tgm_relayout_pages *rows,
				    const struct tgm_layout *l, uint32_t bytes_per_row, int op,
				    const struct tgm_relayout_writer *, uint32_t)
{
	gCpuMoves++;
	gGpu.staleCpuMoves += gGpu.l2Dirty;
	tgm_relayout_cpu(surface, rows, l, bytes_per_row, op == TGM_RELAYOUT_TO_ROWS, modelPage);
}

uint64_t gScratchHandouts;
std::set<tgm_scratch *> gScratchesKept;
int gScratchesReclaimed;
bool gScratchPurged;

extern "C" struct tgm_scratch *tgm_scratch_get(uint64_t bytes, int,
					       struct tgm_relayout_pages *pages)
{
	if (gScratchFails)
		return nullptr;
	static uint64_t next = 0x200000000ull;
	auto *s = new tgm_scratch;
	s->handout = ++gScratchHandouts;
	std::lock_guard<std::mutex> hold(gCleanLock);
	for (uint64_t at = 0; at < bytes; at += 4096, next += 4096) {
		s->pas.push_back(next);
		gCpuLines.insert(next);
	}
	*pages = {s->pas.data(), uint32_t(s->pas.size())};
	return s;
}

// A scratch given back stays kept for a reclaim (the pool's), its bytes as they were unless a test purges them
extern "C" void tgm_scratch_put(struct tgm_scratch *s)
{
	gViews.scratchesPut++;
	gScratchesKept.insert(s);
}

extern "C" struct tgm_scratch *tgm_scratch_reclaim(struct tgm_scratch *s, uint64_t handout,
						   int *intact)
{
	*intact = 0;
	if (!gScratchesKept.count(s) || s->handout != handout)
		return nullptr;
	gScratchesKept.erase(s);
	gScratchesReclaimed++;
	s->handout = ++gScratchHandouts;
	*intact = gScratchPurged ? 0 : 1;
	return s;
}

extern "C" uint64_t tgm_scratch_handout(const struct tgm_scratch *s)
{
	return s->handout;
}

extern "C" void tgm_scratch_discard(struct tgm_scratch *s)
{
	gViews.scratchesDiscarded++;
	delete s;
}

extern "C" void tgm_scratch_stats(uint64_t *held, uint64_t *peak)
{
	*held = *peak = 0;
}

extern "C" void *tgm_scratch_memory(struct tgm_scratch *s)
{
	return s;
}

extern "C" void tgm_scratch_pages(const struct tgm_scratch *s, struct tgm_relayout_pages *pages)
{
	*pages = {s->pas.data(), uint32_t(s->pas.size())};
}

// A kernel mapping of a scratch: its pages' bytes in one buffer, written back to the pages at the unmap.
struct ScratchMap {
	tgm_relayout_pages pages;
	std::vector<uint8_t> bytes;
};

extern "C" void *tgm_scratch_map(struct tgm_scratch *s, void **map)
{
	const tgm_relayout_pages pages = {s->pas.data(), uint32_t(s->pas.size())};
	auto *m = new ScratchMap{pages, gather(pages)};
	*map = m;
	return m->bytes.data();
}

extern "C" void tgm_scratch_unmap(void *map)
{
	auto *m = static_cast<ScratchMap *>(map);
	scatter(m->pages, m->bytes);
	delete m;
}

extern "C" int tgm_cpu_view_of(void *client, uint32_t id, struct tgm_cpu_view *out)
{
	const auto it = gViews.mappings.find({client, id});
	if (it == gViews.mappings.end())
		return 0;
	if (gViews.badMappings.count(client))
		return -1;
	*out = it->second;
	return 1;
}

extern "C" int tgm_cpu_view_remap(const struct tgm_cpu_view *view, void *memory, uint64_t, int)
{
	const bool restore = uintptr_t(memory) & 1;  // ViewModel::surface
	if ((restore && gViews.restoreFails.count(view->client)) ||
	    (!restore && gViews.remapFails > 0)) {
		gViews.remapFails -= !restore;
		return -12;
	}
	gViews.mapped[{view->task, view->address}] = memory;
	gViews.remaps++;
	return 0;
}

extern "C" void *tgm_surface_memory(void *, uint32_t id)
{
	return ViewModel::surface(id);
}

extern "C" void tgm_memory_release(void *)
{
}

extern "C" int tgm_cpu_view_live(const struct tgm_cpu_view *view)
{
	return !gViews.dead.count(view->client);
}

extern "C" void tgm_cpu_view_hold(struct tgm_cpu_view *view, int hold)
{
	gViews.holds[view->client] += hold ? 1 : -1;
}

extern "C" uint64_t tgm_timestamp_ns(void)
{
	return 0;
}

extern "C" void tgm_writers_wait_all(void)
{
	gGpu.waitAll++;
	gGpu.allDone = true;
}

namespace
{
constexpr uint32_t kLock = 2, kUnlock = 3, kReadOnly = 1, kAvoidSync = 2;
constexpr uint32_t kDone = 1;  // a channel whose submissions all completed (FakeGpu: not listed)

std::vector<uint64_t> pages(uint64_t first, uint32_t n)
{
	std::vector<uint64_t> v;
	for (uint32_t i = 0; i < n; i++)
		v.push_back(first + i * 0x2000);
	return v;
}

tgm_coh *add(uint64_t pa, uint32_t surfaceId, const uint64_t *pas, uint32_t npages, int writable,
	     int *covered = nullptr)
{
	int ignored = 0;
	auto *c = tgm_coherency_add(pa, surfaceId, pas, npages, writable, writable,
				    covered ? covered : &ignored);
	EXPECT_EQ(tgm_coherency_pin(c, reinterpret_cast<switch_gpu_backing *>(1), nullptr, nullptr),
		  0);
	return c;
}

tgm_coh *sampled(uint64_t pa, uint32_t surfaceId, const uint64_t *pas, uint32_t npages)
{
	int ignored = 0;
	return tgm_coherency_add(pa, surfaceId, pas, npages, 1, 0, &ignored);
}

class Coherency : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_EQ(tgm_coherency_init(), 0);
		cleanCalls().clear();
		gGpu = FakeGpu{};
		gMoves.clear();
		gMoveFails = false;
		gViews = ViewModel{};
	}
};

TEST_F(Coherency, LockInvalidatesOnlyAfterGpuWrites)
{
	const auto p = pages(0x80000000, 4);
	tgm_coh *c = add(p[0], 7, p.data(), 4, 1);
	ASSERT_NE(c, nullptr);
	tegra_gm20b_iosurface_trap(kLock, 7, 0);
	EXPECT_TRUE(cleanCalls().empty());
	tgm_coherency_wrote(c, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 7, kReadOnly);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tegra_gm20b_iosurface_trap(kLock, 7, 0);
	EXPECT_EQ(cleanCalls().size(), 1u);
	tgm_coherency_put(c);
}

TEST_F(Coherency, ASurfaceTheGpuOnlySamplesIsNeverInvalidatedOnLock)
{
	const auto p = pages(0x8a000000, 4);
	tgm_coh *c = sampled(p[0], 61, p.data(), 4);
	tgm_coherency_wrote(c, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 61, 0);
	EXPECT_TRUE(cleanCalls().empty());
	tgm_coherency_gpu_writes(61);
	tgm_coherency_wrote(c, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 61, 0);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tgm_coherency_put(c);
}

// A SurfaceWrites the client could not deliver: its submissions carry TegraGM20BPushImportsWritten from then on, which
// marks even the imports that said the GPU only samples them.
TEST_F(Coherency, AnUndeclaredWriteStillInvalidatesOnLockOnceTheClientFallsBack)
{
	const auto p = pages(0x8d000000, 3);
	tgm_coh *c = sampled(p[0], 63, p.data(), 3);
	tgm_coherency_wrote(c, 1, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 63, kReadOnly);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tgm_coherency_put(c);
}

TEST_F(Coherency, ARenderedToImportMakesTheSurfaceGpuWrittenForEveryImporter)
{
	const auto p = pages(0x8b000000, 2);
	tgm_coh *reader = sampled(p[0], 62, p.data(), 2);
	tgm_coh *renderer = add(p[0], 62, p.data(), 2, 1);
	ASSERT_EQ(reader, renderer);
	tgm_coherency_wrote(reader, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 62, kReadOnly);
	EXPECT_EQ(cleanCalls().size(), 1u);
	tgm_coherency_put(renderer);
	tgm_coherency_put(reader);
}

TEST_F(Coherency, TrapsReachEveryEntryOfASurface)
{
	std::vector<std::vector<uint64_t>> lists;
	std::vector<tgm_coh *> entries;
	for (uint64_t i = 0; i < 6; i++) {
		lists.push_back(pages(0x8c000000 + (i << 24), 2));
		entries.push_back(add(lists.back()[0], 71, lists.back().data(), 2, 1));
		tgm_coherency_wrote(entries.back(), 0, kDone, 1);
	}
	tegra_gm20b_iosurface_trap(kUnlock, 71, 0);
	EXPECT_EQ(cleanCalls().size(), 6u);
	tegra_gm20b_iosurface_trap(kLock, 71, 0);
	EXPECT_EQ(cleanCalls().size(), 12u);
	for (tgm_coh *c : entries)
		tgm_coherency_put(c);
}

TEST_F(Coherency, UnlockCleansUnlessReadOnly)
{
	const auto p = pages(0x81000000, 3);
	tgm_coh *c = add(p[0], 9, p.data(), 3, 0);
	tegra_gm20b_iosurface_trap(kUnlock, 9, kReadOnly);
	EXPECT_TRUE(cleanCalls().empty());
	tegra_gm20b_iosurface_trap(kUnlock, 9, 0);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tgm_coherency_put(c);
}

TEST_F(Coherency, OtherSurfacesAndUntaggedImportsAreLeftAlone)
{
	const auto p = pages(0x82000000, 2), q = pages(0x83000000, 2);
	tgm_coh *tagged = add(p[0], 11, p.data(), 2, 1);
	tgm_coh *untagged = add(q[0], 0, q.data(), 2, 1);
	tgm_coherency_wrote(tagged, 0, kDone, 1);
	tgm_coherency_wrote(untagged, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kUnlock, 12, 0);
	tegra_gm20b_iosurface_trap(kLock, 12, 0);
	tegra_gm20b_iosurface_trap(kLock, 0, 0);
	tegra_gm20b_iosurface_trap(1, 11, 0);
	EXPECT_TRUE(cleanCalls().empty());
	tgm_coherency_put(tagged);
	tgm_coherency_put(untagged);
}

TEST_F(Coherency, OneEntryPerSurfaceUntilTheLastImportGoes)
{
	const auto small = pages(0x84000000, 2), large = pages(0x84000000, 5);
	tgm_coh *a = add(small[0], 0, small.data(), 2, 0);
	tgm_coh *b = add(large[0], 21, large.data(), 5, 1);
	EXPECT_EQ(a, b);
	tegra_gm20b_iosurface_trap(kUnlock, 21, 0);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], large);
	tgm_coherency_put(b);
	tegra_gm20b_iosurface_trap(kUnlock, 21, 0);
	EXPECT_EQ(cleanCalls().size(), 2u);
	tgm_coherency_put(a);
	tegra_gm20b_iosurface_trap(kUnlock, 21, 0);
	EXPECT_EQ(cleanCalls().size(), 2u);
}

TEST_F(Coherency, OnlyALiveImportOfTheSameSurfaceCoversAnother)
{
	const auto p = pages(0x87000000, 4), more = pages(0x87000000, 6),
		   other = pages(0x88000000, 4);
	int covered = -1;
	tgm_coh *first = add(p[0], 51, p.data(), 4, 1, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coh *early = add(p[0], 51, p.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coherency_cleaned(first);
	tgm_coh *twin = add(p[0], 51, p.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 1);
	tgm_coh *untagged = add(p[0], 0, p.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coh *larger = add(more[0], 51, more.data(), 6, 0, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coh *elsewhere = add(other[0], 51, other.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 0);
	auto moved = p;
	moved[2] = 0x89000000;
	tgm_coh *recycled = add(moved[0], 51, moved.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 0);
	for (tgm_coh *c : {first, early, twin, untagged, larger, elsewhere, recycled})
		tgm_coherency_put(c);
	tgm_coh *again = add(p[0], 51, p.data(), 4, 1, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coherency_put(again);
}

// A 64x64 BGRA8 surface (16 KiB of texels, rows 256 + 64 bytes apart: 20 KiB) over 5 pages
constexpr uint32_t kW = 64, kH = 64, kBpp = 4, kRow = 320;

TEST_F(Coherency, FirstWritingImportAloneTilesWhatTheSurfaceHolds)
{
	const auto p = pages(0x85000000, 5);
	tgm_coh *c = add(p[0], 31, p.data(), 5, 1);
	const tgm_relayout_pages surface = {p.data(), 5};
	std::vector<uint8_t> held(5 * 4096);
	for (size_t i = 0; i < held.size(); i++)
		held[i] = uint8_t(i * 7 + 3);
	scatter(surface, held);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	ASSERT_EQ(gMoves.size(), 1u);
	EXPECT_EQ(gMoves[0].op, TGM_RELAYOUT_TILE_IN_PLACE);
	tgm_layout l;
	ASSERT_TRUE(tgm_layout_tiled(kW, kH, kBpp, &l));
	const std::vector<uint8_t> now = gather(surface);
	std::vector<uint8_t> rows(kRow * kH);
	tgm_layout_read(&l, now.data(), 0, 0, kW, kH, rows.data(), kRow);
	for (uint32_t y = 0; y < kH; y++)
		ASSERT_EQ(0, memcmp(&rows[y * kRow], &held[y * kRow], kW * kBpp)) << y;
	// decided: every later caller, writing or not, at any shape, gets the decision and nothing moves again
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 0), 1);
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH / 2, kBpp, kRow, 1), -1);
	EXPECT_EQ(gMoves.size(), 1u);
	tgm_coherency_put(c);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
}

TEST_F(Coherency, TheLastImportGivesTheSurfaceItsRowsBack)
{
	const auto p = pages(0x85200000, 5);
	const tgm_relayout_pages surface = {p.data(), 5};
	std::vector<uint8_t> held(5 * 4096);
	for (size_t i = 0; i < held.size(); i++)
		held[i] = uint8_t(i * 13 + 1);
	scatter(surface, held);
	tgm_coh *first = add(p[0], 33, p.data(), 5, 1);
	ASSERT_EQ(tgm_coherency_layout(first, kW, kH, kBpp, kRow, 1), 1);
	tgm_coh *second = add(p[0], 33, p.data(), 5, 1);
	tgm_coherency_put(first);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_EQ(gMoves.size(), 1u);
	tgm_coherency_put(second);
	ASSERT_EQ(gMoves.size(), 2u);
	EXPECT_EQ(gMoves[1].op, TGM_RELAYOUT_UNTILE_IN_PLACE);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	const std::vector<uint8_t> now = gather(surface);
	for (uint32_t y = 0; y < kH; y++)
		ASSERT_EQ(0, memcmp(&now[y * kRow], &held[y * kRow], kW * kBpp)) << y;
	// a new GPU import decides afresh over the rows
	tgm_coh *again = add(p[0], 33, p.data(), 5, 1);
	EXPECT_EQ(tgm_coherency_layout(again, kW, kH, kBpp, kRow, 1), 1);
	EXPECT_EQ(gMoves.back().op, TGM_RELAYOUT_TILE_IN_PLACE);
	tgm_coherency_put(again);
}

// The display scanning a surface in place holds its layout: its client's last import going (the client died) moves
// nothing; the display letting go does.
TEST_F(Coherency, TheDisplaysHoldKeepsTheLayoutPastTheLastImport)
{
	const auto p = pages(0x85300000, 5);
	int tiled = -1;
	EXPECT_EQ(tegra_gm20b_surface_hold(p[0], &tiled), nullptr);
	EXPECT_EQ(tiled, 0);
	tgm_coh *c = add(p[0], 34, p.data(), 5, 1);
	EXPECT_EQ(tegra_gm20b_surface_hold(p[0], &tiled),
		  nullptr);  // undecided: nothing to hold yet
	ASSERT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	void *hold = tegra_gm20b_surface_hold(p[0], &tiled);
	ASSERT_NE(hold, nullptr);
	EXPECT_EQ(tiled, 1);
	const size_t moves = gMoves.size();
	tgm_coherency_put(c);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_EQ(gMoves.size(), moves);
	tegra_gm20b_surface_unhold(hold);
	ASSERT_EQ(gMoves.size(), moves + 1);
	EXPECT_EQ(gMoves.back().op, TGM_RELAYOUT_UNTILE_IN_PLACE);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
}

TEST_F(Coherency, TilingFollowsTheSurfacesWritersOnTheGpu)
{
	const auto p = pages(0x85100000, 5);
	tgm_coh *c = add(p[0], 32, p.data(), 5, 1);
	gGpu.completed[5] = 10;
	tgm_coherency_wrote(c, 0, 5, 12);
	tgm_coherency_wrote(c, 0, kDone, 1);
	ASSERT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	ASSERT_EQ(gMoves.size(), 1u);
	ASSERT_EQ(gMoves[0].writers.size(), 1u);
	EXPECT_EQ(gMoves[0].writers[0].uid, 5u);
	EXPECT_EQ(gMoves[0].writers[0].seqno, 12u);
	EXPECT_TRUE(gGpu.waits.empty());  // ordered on the GPU, no CPU wait
	tgm_coherency_put(c);
}

TEST_F(Coherency, TilingMovesStartOnAScratchWithNoCpuLines)
{
	const auto p = pages(0x85300000, 5);
	tgm_coh *first = add(p[0], 34, p.data(), 5, 1);
	ASSERT_EQ(tgm_coherency_layout(first, kW, kH, kBpp, kRow, 1), 1);
	tgm_coherency_put(first);
	ASSERT_EQ(gMoves.size(), 2u);
	EXPECT_EQ(gMoves[0].op, TGM_RELAYOUT_TILE_IN_PLACE);
	EXPECT_TRUE(gMoves[0].rowsClean);
	EXPECT_EQ(gMoves[1].op, TGM_RELAYOUT_UNTILE_IN_PLACE);
	EXPECT_TRUE(gMoves[1].rowsClean);
}

TEST_F(Coherency, FailedFinalUntileKeepsLayoutUntilSuccessfulRetry)
{
	const auto p = pages(0x85400000, 5);
	auto *c = add(p[0], 35, p.data(), 5, 1);
	ASSERT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	gMoveFails = true;
	tgm_coherency_put(c);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	auto *next = add(p[0], 35, p.data(), 5, 1);
	EXPECT_EQ(next, c);
	EXPECT_EQ(tgm_coherency_layout(next, kW, kH, kBpp, kRow, 1), 1);
	gMoveFails = false;
	tgm_coherency_put(next);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
}

TEST_F(Coherency, ParkedLayoutRetainsActualSurfaceIdentityUntilUntile)
{
	const auto p = pages(0x85700000, 5);
	auto *c = add(p[0], 38, p.data(), 5, 1);
	unsigned releases = 0;
	const auto released = [](void *owner) {
		++*static_cast<unsigned *>(owner);
	};
	ASSERT_EQ(tgm_coherency_pin(c, reinterpret_cast<switch_gpu_backing *>(1), &releases,
				    released),
		  0);
	ASSERT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	gScratchFails = true;
	tgm_coherency_put(c);
	EXPECT_EQ(releases, 0u);
	auto *held = tgm_coh_find_pa(p[0]);
	gScratchFails = false;
	tgm_coherency_put(held);
	EXPECT_EQ(releases, 1u);
}

TEST_F(Coherency, ImportArrivingBeforeFinalUntileKeepsItsLayout)
{
	const auto p = pages(0x85600000, 5);
	auto *first = add(p[0], 37, p.data(), 5, 1);
	ASSERT_EQ(tgm_coherency_layout(first, kW, kH, kBpp, kRow, 1), 1);
	auto *second = add(p[0], 37, p.data(), 5, 1);
	EXPECT_EQ(tgm_tiled_untile(first), 0);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	tgm_coherency_put(first);
	EXPECT_EQ(tgm_coherency_layout(second, kW, kH, kBpp, kRow, 1), 1);
	tgm_coherency_put(second);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
}

TEST_F(Coherency, ScratchFailureAtFinalDropKeepsRegistryReachable)
{
	const auto p = pages(0x85500000, 5);
	auto *c = add(p[0], 36, p.data(), 5, 1);
	ASSERT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	gScratchFails = true;
	tgm_coherency_put(c);
	ASSERT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	auto *held = tgm_coh_find_pa(p[0]);
	ASSERT_EQ(held, c);
	gScratchFails = false;
	tgm_coherency_put(held);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
}

TEST_F(Coherency, PitchForGoodUnlessTheFirstAskerWritesItAlone)
{
	const auto a = pages(0x86000000, 5), b = pages(0x86100000, 5), d = pages(0x86200000, 5),
		   e = pages(0x86300000, 5);
	tgm_coh *sampled = add(a[0], 41, a.data(), 5, 1);
	EXPECT_EQ(tgm_coherency_layout(sampled, kW, kH, kBpp, kRow, 0), 0);
	EXPECT_EQ(tgm_coherency_layout(sampled, kW, kH, kBpp, kRow, 1), 0);
	tgm_coh *first = add(b[0], 42, b.data(), 5, 1), *second = add(b[0], 42, b.data(), 5, 1);
	EXPECT_EQ(first, second);
	EXPECT_EQ(tgm_coherency_layout(first, kW, kH, kBpp, kRow, 1), 0);
	tgm_coh *odd = add(d[0], 43, d.data(), 5, 1);
	EXPECT_EQ(tgm_coherency_layout(odd, kW, kH, 3, kRow, 1), 0);
	tgm_coh *small = add(e[0], 44, e.data(), 1, 1);
	EXPECT_EQ(tgm_coherency_layout(small, kW, kH, kBpp, kRow, 1), 0);
	EXPECT_TRUE(gMoves.empty());
	for (const auto &p : {a, b, d, e})
		EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	for (tgm_coh *c : {sampled, first, second, odd, small})
		tgm_coherency_put(c);
}

TEST_F(Coherency, AFailedTilingMoveLeavesItPitch)
{
	const auto p = pages(0x87000000, 5);
	tgm_coh *c = add(p[0], 51, p.data(), 5, 1);
	gRefusals.clear();
	gMoveFails = true;
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 0);
	gMoveFails = false;
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 0);
	EXPECT_EQ(gMoves.size(), 1u);
	EXPECT_EQ(gRefusals, decltype(gRefusals)({{"iosurface-layout", "tile-move-failed"}}));
	tgm_coherency_put(c);
}

// No scratch: no move is tried, the refusal names why, and the surface is pitch for good
TEST_F(Coherency, NoScratchForTheTilingMoveLeavesItPitch)
{
	const auto p = pages(0x87100000, 5);
	tgm_coh *c = add(p[0], 52, p.data(), 5, 1);
	gRefusals.clear();
	gScratchFails = true;
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 0);
	gScratchFails = false;
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_TRUE(gMoves.empty());
	EXPECT_EQ(gRefusals, decltype(gRefusals)({{"iosurface-layout", "tile-no-scratch"}}));
	tgm_coherency_put(c);
}

TEST_F(Coherency, ConcurrentAskersGetOneDecision)
{
	const auto p = pages(0x88000000, 5);
	tgm_coh *c = add(p[0], 61, p.data(), 5, 1);
	std::atomic<int> tiled{0};
	std::vector<std::thread> askers;
	for (int i = 0; i < 4; i++)
		askers.emplace_back([&] {
			tiled += tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1);
		});
	for (auto &t : askers)
		t.join();
	EXPECT_EQ(tiled.load(), 4);
	EXPECT_EQ(gMoves.size(), 1u);
	tgm_coherency_put(c);
}

TEST_F(Coherency, TrapsRaceReleaseWithoutUseAfterFree)
{
	std::atomic<bool> stop{false};
	std::thread trapper([&] {
		while (!stop.load()) {
			tegra_gm20b_iosurface_trap(kUnlock, 41, 0);
			tegra_gm20b_iosurface_trap(kLock, 41, 0);
		}
	});
	for (int i = 0; i < 2000; i++) {
		const auto p = pages(0x86000000 + (uint64_t(i % 8) << 24), 8);
		tgm_coh *c = add(p[0], 41, p.data(), 8, 1);
		tgm_coherency_wrote(c, 0, kDone, 1);
		tgm_coherency_put(c);
	}
	stop.store(true);
	trapper.join();
}
}  // namespace

// Apple's IOSurfaceLock synchronises with the GPU: a lock of a surface whose writer is still running sleeps until it
// completed, then the trap invalidates what it wrote.
TEST_F(Coherency, ALockBeforeTheWriterCompletesWaitsForIt)
{
	const auto p = pages(0x9a000000, 2);
	tgm_coh *c = add(p[0], 71, p.data(), 2, 1);
	gGpu.completed[5] = 9;
	tgm_coherency_wrote(c, 0, 5, 10);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(71, kReadOnly), 0);
	ASSERT_EQ(gGpu.waits.size(), 1u);
	EXPECT_EQ(gGpu.waits[0], std::make_pair(5u, 10u));
	tegra_gm20b_iosurface_trap(kLock, 71, kReadOnly);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(71, 0), 0);
	EXPECT_EQ(gGpu.waits.size(), 1u);
	tgm_coherency_put(c);
}

TEST_F(Coherency, ALockAfterTheWriterCompletedDoesNotWait)
{
	const auto p = pages(0x9b000000, 2);
	tgm_coh *c = add(p[0], 72, p.data(), 2, 1);
	gGpu.completed[5] = 9;
	tgm_coherency_wrote(c, 0, 5, 9);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(72, 0), 0);
	EXPECT_TRUE(gGpu.waits.empty());
	tegra_gm20b_iosurface_trap(kLock, 72, 0);
	EXPECT_EQ(cleanCalls().size(), 1u);
	tgm_coherency_put(c);
}

// kIOSurfaceLockAvoidSync: a lock that would have to wait fails (kIOReturnCannotLock) without waiting.
TEST_F(Coherency, AnAvoidSyncLockWithWritesPendingCannotLock)
{
	const auto p = pages(0x9c000000, 2);
	tgm_coh *c = add(p[0], 73, p.data(), 2, 1);
	gGpu.completed[6] = 1;
	tgm_coherency_wrote(c, 0, 6, 2);
	EXPECT_NE(tegra_gm20b_iosurface_lock_will(73, kAvoidSync | kReadOnly), 0);
	EXPECT_TRUE(gGpu.waits.empty());
	gGpu.completed[6] = 2;
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(73, kAvoidSync | kReadOnly), 0);
	tgm_coherency_put(c);
}

// A killed channel's fence is released: its writes count as complete.
TEST_F(Coherency, AKilledWritersChannelCountsAsComplete)
{
	const auto p = pages(0x9d000000, 2);
	tgm_coh *c = add(p[0], 74, p.data(), 2, 1);
	gGpu.completed[8] = 0;
	tgm_coherency_wrote(c, 0, 8, 4);
	gGpu.killed.insert(8);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(74, 0), 0);
	EXPECT_TRUE(gGpu.waits.empty());
	tgm_coherency_put(c);
}

// A later submission of the same channel takes its writer's slot; more channels than slots make a lock wait for all.
TEST_F(Coherency, WritersPerChannelAndTooManyChannels)
{
	const auto p = pages(0x9e000000, 2);
	tgm_coh *c = add(p[0], 75, p.data(), 2, 1);
	gGpu.completed[2] = 0;
	tgm_coherency_wrote(c, 0, 2, 1);
	tgm_coherency_wrote(c, 0, 2, 3);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(75, 0), 0);
	ASSERT_EQ(gGpu.waits.size(), 1u);
	EXPECT_EQ(gGpu.waits[0].second, 3u);
	for (uint32_t uid = 10; uid < 16; uid++) {
		gGpu.completed[uid] = 0;
		tgm_coherency_wrote(c, 0, uid, 1);
	}
	gGpu.allDone = false;
	EXPECT_NE(tegra_gm20b_iosurface_lock_will(75, kAvoidSync), 0);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(75, 0), 0);
	EXPECT_EQ(gGpu.waitAll, 1);
	tgm_coherency_put(c);
}
