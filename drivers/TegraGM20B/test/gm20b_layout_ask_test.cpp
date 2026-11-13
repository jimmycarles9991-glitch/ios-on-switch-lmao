// SPDX-License-Identifier: MIT
// SurfaceLayout as the user client asks it (TegraGM20BUserClient.cpp surfaceLayoutInfo), over gm20b_tiled_model.h: the
// asker holds its import's entry (tgm_coh_hold_asker) and drops the connection's lock across tgm_coherency_layout. That
// hold keeps the entry alive and is not another user of the surface: a first writing import with no other decides
// block-linear, one another client imported first stays pitch, and the hold goes on every outcome.
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BLayoutFit.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.h"
#include "drivers/TegraGM20B/TegraGM20BTiled.h"
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"
#include "gm20b_tiled_model.h"

namespace
{
constexpr uint32_t kW = 64, kH = 64, kBpp = 4, kRow = 320, kPages = 5;

std::vector<uint64_t> pages(uint64_t first)
{
	std::vector<uint64_t> v;
	for (uint32_t i = 0; i < kPages; i++)
		v.push_back(first + i * 0x2000);
	return v;
}

tgm_coh *import(const std::vector<uint64_t> &p, uint32_t surfaceId)
{
	int covered = 0;
	tgm_coh *c = tgm_coherency_add(p[0], surfaceId, p.data(), kPages, 1, 1, &covered);
	EXPECT_EQ(tgm_coherency_pin(c, reinterpret_cast<switch_gpu_backing *>(1), nullptr, nullptr),
		  0);
	return c;
}

int ask(tgm_coh *c, int writes, uint32_t bpp = kBpp)
{
	tgm_coh_hold_asker(c);
	const int layout = tgm_coherency_layout(c, kW, kH, bpp, kRow, writes);
	tgm_coh_put_asker(c);
	return layout;
}

// No reference to the surface's entry is left: every import and hold has been put.
bool gone(uint64_t pa)
{
	tgm_coh *c = tgm_coh_find_pa(pa);
	if (c)
		tgm_coherency_put(c);
	return c == nullptr;
}

class LayoutAsk : public ::testing::Test
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
	}
};

TEST_F(LayoutAsk, AFirstWritingImportAskingUnderItsHoldDecidesBlockLinear)
{
	const auto p = pages(0xc0000000);
	tgm_coh *c = import(p, 201);
	EXPECT_EQ(ask(c, 1), 1);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	ASSERT_EQ(gMoves.size(), 1u);
	EXPECT_EQ(gMoves[0].op, TGM_RELAYOUT_TILE_IN_PLACE);
	EXPECT_EQ(ask(c, 0), 1);
	EXPECT_EQ(gMoves.size(), 1u);
	tgm_coherency_put(c);
	ASSERT_EQ(gMoves.size(), 2u);
	EXPECT_EQ(gMoves[1].op, TGM_RELAYOUT_UNTILE_IN_PLACE);
	EXPECT_TRUE(gone(p[0]));
}

// Two threads of one connection asking at once: both holds are the asker's own.
TEST_F(LayoutAsk, EveryHoldOfTheAskersIsItsOwn)
{
	const auto p = pages(0xc0100000);
	tgm_coh *c = import(p, 202);
	tgm_coh_hold_asker(c);
	tgm_coh_hold_asker(c);
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	tgm_coh_put_asker(c);
	tgm_coh_put_asker(c);
	EXPECT_EQ(gMoves.size(), 1u);
	tgm_coherency_put(c);
	EXPECT_TRUE(gone(p[0]));
}

// Another client imported the surface before anyone asked: it binds it pitch, so the surface is pitch for good, for the
// asker and for that client's own later ask.
TEST_F(LayoutAsk, AnotherClientsEarlierImportKeepsItPitch)
{
	const auto p = pages(0xc0200000);
	tgm_coh *other = import(p, 203);
	tgm_coh *mine = import(p, 203);
	ASSERT_EQ(other, mine);
	EXPECT_EQ(ask(mine, 1), 0);
	EXPECT_EQ(ask(other, 1), 0);
	EXPECT_TRUE(gMoves.empty());
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	tgm_coherency_put(mine);
	tgm_coherency_put(other);
	EXPECT_TRUE(gone(p[0]));
}

// Each pitch outcome puts the asker's hold: only the import's reference is left for it to put.
TEST_F(LayoutAsk, ThePitchOutcomesPutTheHold)
{
	const auto sampled = pages(0xc0300000), shape = pages(0xc0400000),
		   failed = pages(0xc0500000);
	tgm_coh *a = import(sampled, 204);
	EXPECT_EQ(ask(a, 0), 0);
	tgm_coh *b = import(shape, 205);
	EXPECT_EQ(ask(b, 1, 3), 0);
	tgm_coh *d = import(failed, 206);
	gMoveFails = true;
	EXPECT_EQ(ask(d, 1), 0);
	gMoveFails = false;
	for (tgm_coh *c : {a, b, d})
		tgm_coherency_put(c);
	for (const auto &p : {sampled, shape, failed})
		EXPECT_TRUE(gone(p[0]));
}

// The asker's BoFree running while its decision is under way (the connection's lock is dropped): the hold keeps the
// entry until the asker puts it, then nothing is left.
TEST_F(LayoutAsk, AnImportFreedDuringItsAskIsReleasedByTheHold)
{
	const auto p = pages(0xc0600000);
	tgm_coh *c = import(p, 207);
	tgm_coh_hold_asker(c);
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 1);
	tgm_coherency_put(c);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_EQ(gMoves.size(), 1u);
	tgm_coh_put_asker(c);
	ASSERT_EQ(gMoves.size(), 2u);
	EXPECT_EQ(gMoves[1].op, TGM_RELAYOUT_UNTILE_IN_PLACE);
	EXPECT_TRUE(gone(p[0]));
}

// Freed before the decision ran: no import is left to write it, so it is not tiled, and the hold is the last reference.
TEST_F(LayoutAsk, AnImportFreedBeforeItsDecisionLeavesItPitch)
{
	const auto p = pages(0xc0700000);
	tgm_coh *c = import(p, 208);
	tgm_coh_hold_asker(c);
	tgm_coherency_put(c);
	EXPECT_EQ(tgm_coherency_layout(c, kW, kH, kBpp, kRow, 1), 0);
	EXPECT_TRUE(gMoves.empty());
	tgm_coh_put_asker(c);
	EXPECT_TRUE(gone(p[0]));
}
}  // namespace

namespace
{
// backboardd's 1280x720 BGRA surfaces: 3,686,400 bytes, 900 pages of the model's 4 KiB
constexpr uint32_t kFrameW = 1280, kFrameH = 720, kFrameRow = 5120, kFramePages = 900;

tgm_coh *importFrame(uint64_t first, uint32_t surfaceId, std::vector<uint64_t> &p)
{
	for (uint32_t i = 0; i < kFramePages; i++)
		p.push_back(first + i * 0x2000);
	int covered = 0;
	tgm_coh *c = tgm_coherency_add(p[0], surfaceId, p.data(), kFramePages, 1, 1, &covered);
	EXPECT_EQ(tgm_coherency_pin(c, reinterpret_cast<switch_gpu_backing *>(1), nullptr, nullptr),
		  0);
	return c;
}
}  // namespace

TEST_F(LayoutAsk, AFrameNilWouldPadPastItsAllocationTakesTheTallestBlocksThatFit)
{
	std::vector<uint64_t> p;
	tgm_coh *c = importFrame(0xd0000000, 301, p);
	tgm_coh_hold_asker(c);
	EXPECT_EQ(tgm_coherency_layout_fitted(c, kFrameW, kFrameH, 4, kFrameRow, 1), 1);
	tgm_coh_put_asker(c);
	tgm_layout l = {};
	ASSERT_EQ(tegra_gm20b_surface_layout(p[0], &l), 1);
	EXPECT_EQ(l.y_log2, 1);
	EXPECT_EQ(l.rows, kFrameH);
	EXPECT_EQ(l.row_bytes, kFrameRow);
	EXPECT_EQ(l.bytes, uint64_t(kFramePages) << 12);
	ASSERT_EQ(gMoves.size(), 1u);
	EXPECT_EQ(gMoves[0].op, TGM_RELAYOUT_TILE_IN_PLACE);
	tgm_coherency_put(c);
	EXPECT_TRUE(gone(p[0]));
}

TEST_F(LayoutAsk, AnAskerThatTakesNilsBlockHeightLeavesSuchAFramePitch)
{
	std::vector<uint64_t> p;
	tgm_coh *c = importFrame(0xd8000000, 302, p);
	tgm_coh_hold_asker(c);
	EXPECT_EQ(tgm_coherency_layout(c, kFrameW, kFrameH, 4, kFrameRow, 1), 0);
	tgm_coh_put_asker(c);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	EXPECT_TRUE(gMoves.empty());
	tgm_coherency_put(c);
}

TEST(LayoutFit, TheTallestBlocksAtMostNilsThatFitTheAllocation)
{
	tgm_layout l = {};
	ASSERT_TRUE(tgm_layout_tiled_within(720, 1280, 4, 720 * 1280 * 4, &l));
	EXPECT_EQ(l.y_log2, 4);
	EXPECT_TRUE(tgm_layout_nil_height(&l));
	ASSERT_TRUE(tgm_layout_tiled_within(1280, 720, 4, 1280 * 720 * 4, &l));
	EXPECT_EQ(l.y_log2, 1);
	EXPECT_FALSE(tgm_layout_nil_height(&l));
	ASSERT_TRUE(tgm_layout_tiled_within(136, 136, 8, 163840, &l));
	EXPECT_EQ(l.y_log2, 1);
	EXPECT_EQ(l.rows, 144u);
	EXPECT_FALSE(tgm_layout_tiled_within(100, 100, 4, 100 * 100 * 4, &l));
}
