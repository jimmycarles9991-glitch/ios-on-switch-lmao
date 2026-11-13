// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdio>
#include <memory>
#include <string>
#include "../TegraGM20BBindHistory.h"

namespace
{
constexpr uint64_t kMs = 1000000;

tgm_bind_note note(uint8_t op, uint64_t va, uint64_t range, uint64_t handle, uint64_t ns,
		   uint32_t submitted)
{
	tgm_bind_note n = {};
	n.op = op;
	n.va = va;
	n.range = range;
	n.handle = handle;
	n.bo_bytes = range;
	n.ns = ns;
	n.kind = 0xfe;
	n.submitted[2] = submitted;
	return n;
}

std::string line(const tgm_bind_note &n, uint64_t kill, uint32_t failed, uint64_t now = 0)
{
	char buf[256];
	tgm_bind_note_format(buf, sizeof(buf), &n, now ? now : kill, kill, 2, failed);
	return buf;
}

TEST(GM20BBindHistory, UnmapUnderTheFailedJobIsNamed)
{
	auto h = std::make_unique<tgm_bind_history>();
	h->written = 0;
	const tgm_bind_note mapped =
		note(TGM_BIND_MAP, 0x3ef3c00000, 0x390000, 1649, 100 * kMs, 2410);
	const tgm_bind_note freed =
		note(TGM_BIND_FREED, 0x3ef3c00000, 0x390000, 1649, 140 * kMs, 2414);
	tgm_bind_note_add(h.get(), &mapped);
	tgm_bind_note_add(h.get(), &freed);
	uint32_t near[4];
	ASSERT_EQ(tgm_bind_history_near(h.get(), 0x3ef3ebd000, TGM_BIND_SLACK, near, 4), 2u);
	EXPECT_EQ(line(h->notes[near[0]], 152 * kMs, 2413),
		  "12 ms before the kill: unmapped by its BoFree 0x3ef3c00000+0x390000 of BO 1649 "
		  "(3735552 bytes, alloc at 0x0), kind 0xfe, the failed job submitted");
	EXPECT_EQ(line(h->notes[near[1]], 152 * kMs, 2413),
		  "52 ms before the kill: mapped 0x3ef3c00000+0x390000 of BO 1649 (3735552 bytes, "
		  "alloc at 0x0), kind 0xfe, before the failed job");
}

TEST(GM20BBindHistory, AWriteJustPastARangeIsNearIt)
{
	const tgm_bind_note n = note(TGM_BIND_MAP, 0x10000, 0x20000, 1, 0, 0);
	EXPECT_TRUE(tgm_bind_note_near(&n, 0x30000, TGM_BIND_SLACK));
	EXPECT_TRUE(tgm_bind_note_near(&n, 0x3ffff, TGM_BIND_SLACK));
	EXPECT_FALSE(tgm_bind_note_near(&n, 0x40000, TGM_BIND_SLACK));
	EXPECT_TRUE(tgm_bind_note_near(&n, 0x0, TGM_BIND_SLACK));
	const tgm_bind_note low = note(TGM_BIND_MAP, 0x1000, 0x1000, 1, 0, 0);
	EXPECT_TRUE(tgm_bind_note_near(&low, 0x0, TGM_BIND_SLACK));
	EXPECT_FALSE(tgm_bind_note_near(&low, 0x12000, TGM_BIND_SLACK));
}

TEST(GM20BBindHistory, OnlyTheLastNotesAreKeptNewestFirst)
{
	auto h = std::make_unique<tgm_bind_history>();
	h->written = 0;
	for (uint32_t i = 0; i < TGM_BIND_NOTES + 10; i++) {
		const tgm_bind_note n = note(TGM_BIND_UNMAP, 0x100000, 0x1000, i, i, 0);
		tgm_bind_note_add(h.get(), &n);
	}
	uint32_t near[TGM_BIND_NOTES + 10];
	const uint32_t n = tgm_bind_history_near(h.get(), 0x100000, 0, near, TGM_BIND_NOTES + 10);
	ASSERT_EQ(n, TGM_BIND_NOTES);
	EXPECT_EQ(h->notes[near[0]].handle, TGM_BIND_NOTES + 9);
	EXPECT_EQ(h->notes[near[n - 1]].handle, 10u);
	uint32_t two[2];
	EXPECT_EQ(tgm_bind_history_near(h.get(), 0x100000, 0, two, 2), 2u);
	EXPECT_EQ(h->notes[two[1]].handle, TGM_BIND_NOTES + 8);
}

TEST(GM20BBindHistory, ARefusedMapAndASurfaceImportAreNamed)
{
	tgm_bind_note n = note(TGM_BIND_REFUSED, 0x2000, 0x1000, 7, 0, 0);
	n.origin = TGM_BIND_IMPORT;
	n.surface = 23;
	n.bo_offset = 0x4000;
	EXPECT_EQ(line(n, 3 * kMs, 0),
		  "3 ms before the kill: map refused 0x2000+0x1000 of BO 7 (4096 bytes, import of "
		  "surface 23 at 0x4000), kind 0xfe");
}

TEST(GM20BBindHistory, ANoteAfterTheKillIsTheDriversCleanup)
{
	const tgm_bind_note n = note(TGM_BIND_UNMAP, 0x2000, 0x1000, 7, 15 * kMs, 40);
	EXPECT_EQ(line(n, 10 * kMs, 30),
		  "5 ms after the kill: unmapped 0x2000+0x1000 of BO 7 (4096 bytes, alloc at 0x0), "
		  "kind 0xfe");
}

TEST(GM20BBindHistory, WithNoKillTimeANoteIsAgedFromNowAndNamesNoJob)
{
	const tgm_bind_note n = note(TGM_BIND_FREED, 0x2000, 0x1000, 7, 15 * kMs, 40);
	EXPECT_EQ(line(n, 0, 30, 20 * kMs),
		  "5 ms ago: unmapped by its BoFree 0x2000+0x1000 of BO 7 (4096 bytes, alloc at "
		  "0x0), kind 0xfe");
}

TEST(GM20BBindHistory, TheSpanIsTheOldestKeptNotesAge)
{
	auto h = std::make_unique<tgm_bind_history>();
	h->written = 0;
	EXPECT_EQ(tgm_bind_history_span_ns(h.get(), 100), 0u);
	for (uint32_t i = 0; i < TGM_BIND_NOTES + 3; i++) {
		const tgm_bind_note n = note(TGM_BIND_MAP, 0x1000, 0x1000, i, 1000 + i, 0);
		tgm_bind_note_add(h.get(), &n);
		const uint64_t oldest = i < TGM_BIND_NOTES ? 1000 : 1000 + i + 1 - TGM_BIND_NOTES;
		EXPECT_EQ(tgm_bind_history_span_ns(h.get(), 5000), 5000 - oldest);
	}
}

TEST(GM20BBindHistory, AnUnknownSlotOrJobSaysNothingOfIt)
{
	const tgm_bind_note n = note(TGM_BIND_UNMAP, 0x2000, 0x1000, 7, 0, 0);
	EXPECT_STREQ(tgm_bind_note_job(&n, 2, 5), "");
	const tgm_bind_note m = note(TGM_BIND_UNMAP, 0x2000, 0x1000, 7, 0, 9);
	EXPECT_STREQ(tgm_bind_note_job(&m, 2, 0), "");
	EXPECT_STREQ(tgm_bind_note_job(&m, TGM_BIND_CHANNELS, 5), "");
	EXPECT_STREQ(tgm_bind_note_job(&m, 2, 9), ", the failed job submitted");
	EXPECT_STREQ(tgm_bind_note_job(&m, 2, 10), ", before the failed job");
}
}  // namespace
