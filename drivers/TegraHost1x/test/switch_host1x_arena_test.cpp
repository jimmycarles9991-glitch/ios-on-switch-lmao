// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <map>
#include <utility>
#include "../SwitchHost1xArena.h"
#include "../SwitchHost1xGatherStats.h"

namespace {
constexpr uint32_t kSyncpt = 160;

// The engine's syncpoint as reclaim reads it.
struct Syncpoint {
    uint32_t value = 0;

    auto reached()
    {
        return [this](uint32_t id, uint32_t threshold) {
            EXPECT_EQ(id, kSyncpt);
            return static_cast<int32_t>(value - threshold) >= 0;
        };
    }
};

uint32_t submit(SwitchHost1xArena &arena, uint32_t capacity, uint32_t used, uint32_t threshold, bool fenced = true)
{
    const uint32_t at = arena.reserve(capacity);
    EXPECT_NE(at, SwitchHost1xArena::kNone);
    EXPECT_TRUE(arena.commit(at, used, fenced, kSyncpt, threshold));
    return at;
}

TEST(Host1xArena, JobsFollowEachOtherAndGiveBackWhatTheyDidNotUse)
{
    SwitchHost1xArena arena(1000);
    EXPECT_EQ(submit(arena, 300, 100, 1), 0u);
    EXPECT_EQ(submit(arena, 300, 50, 2), 100u);
    EXPECT_EQ(arena.held(), 150u);
    EXPECT_EQ(arena.jobs(), 2u);
}

TEST(Host1xArena, AStreamThatDoesNotFitBeforeTheEndStartsAtTheBeginningOnceItIsFree)
{
    SwitchHost1xArena arena(1000);
    Syncpoint syncpoint;
    submit(arena, 400, 400, 1);
    submit(arena, 400, 400, 2);
    EXPECT_EQ(arena.reserve(400), SwitchHost1xArena::kNone) << "200 left at the end, 0 before the oldest job";
    syncpoint.value = 1;
    EXPECT_EQ(arena.reclaim(syncpoint.reached()), 1u);
    EXPECT_EQ(submit(arena, 400, 400, 3), 0u) << "the 200 words at the end are skipped";
    EXPECT_EQ(arena.held(), 1000u) << "the skipped words stay held until the job before them retires";
    EXPECT_FALSE(arena.fits(1));
    syncpoint.value = 2;
    arena.reclaim(syncpoint.reached());
    EXPECT_EQ(arena.held(), 400u);
    EXPECT_EQ(submit(arena, 600, 600, 4), 400u);
}

TEST(Host1xArena, RoomComesBackOnlyInFenceOrder)
{
    SwitchHost1xArena arena(1000);
    Syncpoint syncpoint;
    submit(arena, 300, 300, 1);
    submit(arena, 300, 300, 2);
    submit(arena, 300, 300, 3);
    EXPECT_EQ(arena.reclaim(syncpoint.reached()), 0u) << "no fence passed";
    syncpoint.value = 2;
    EXPECT_EQ(arena.reclaim(syncpoint.reached()), 2u);
    EXPECT_EQ(arena.jobs(), 1u);
    EXPECT_EQ(arena.held(), 300u);
    ASSERT_NE(arena.oldestFenced(), nullptr);
    EXPECT_EQ(arena.oldestFenced()->threshold, 3u);
}

TEST(Host1xArena, AStreamWithoutAFenceRetiresWithTheNextFencedJob)
{
    SwitchHost1xArena arena(1000);
    Syncpoint syncpoint;
    submit(arena, 3, 3, 0, false);
    EXPECT_EQ(arena.oldestFenced(), nullptr);
    submit(arena, 100, 100, 1);
    EXPECT_EQ(arena.oldestFenced()->threshold, 1u);
    EXPECT_EQ(arena.reclaim(syncpoint.reached()), 0u);
    syncpoint.value = 1;
    EXPECT_EQ(arena.reclaim(syncpoint.reached()), 2u);
    EXPECT_EQ(arena.held(), 0u);
}

TEST(Host1xArena, FencesCompareAcrossTheSyncpointWrap)
{
    SwitchHost1xArena arena(1000);
    Syncpoint syncpoint{0xfffffffeu};
    submit(arena, 10, 10, 0xffffffffu);
    submit(arena, 10, 10, 1);
    syncpoint.value = 0xffffffffu;
    EXPECT_EQ(arena.reclaim(syncpoint.reached()), 1u);
    syncpoint.value = 1;
    EXPECT_EQ(arena.reclaim(syncpoint.reached()), 1u);
}

TEST(Host1xArena, OneReservationAtATimeAndCommitStaysInsideIt)
{
    SwitchHost1xArena arena(1000);
    const uint32_t at = arena.reserve(100);
    ASSERT_EQ(at, 0u);
    EXPECT_TRUE(arena.reserved());
    EXPECT_EQ(arena.reserve(10), SwitchHost1xArena::kNone);
    EXPECT_FALSE(arena.fits(10));
    EXPECT_FALSE(arena.commit(at + 1, 100, true, kSyncpt, 1)) << "past the reservation";
    EXPECT_FALSE(arena.commit(at, 0, true, kSyncpt, 1));
    EXPECT_TRUE(arena.commit(at + 3, 97, true, kSyncpt, 1)) << "a stream may start inside its reservation";
    EXPECT_FALSE(arena.reserved());
    EXPECT_EQ(arena.held(), 100u);
    EXPECT_EQ(arena.reserve(10), 100u);
    arena.cancel();
    EXPECT_FALSE(arena.reserved());
    EXPECT_EQ(arena.held(), 100u) << "a cancelled reservation holds nothing";
}

TEST(Host1xArena, TheEntryLimitBoundsTheJobsInFlight)
{
    SwitchHost1xArena arena(1000);
    Syncpoint syncpoint;
    for (uint32_t i = 0; i < SwitchHost1xArena::kEntries; ++i)
        submit(arena, 1, 1, i + 1);
    EXPECT_EQ(arena.reserve(1), SwitchHost1xArena::kNone);
    syncpoint.value = 1;
    arena.reclaim(syncpoint.reached());
    EXPECT_NE(arena.reserve(1), SwitchHost1xArena::kNone);
}

TEST(Host1xArena, ADroppedChannelHoldsNothing)
{
    SwitchHost1xArena arena(1000);
    submit(arena, 500, 500, 1);
    arena.drop();
    EXPECT_EQ(arena.held(), 0u);
    EXPECT_EQ(arena.reserve(1000), 0u);
}

TEST(Host1xArena, ManyWrapsKeepEveryLiveStreamDisjoint)
{
    SwitchHost1xArena arena(1024);
    Syncpoint syncpoint;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> live;  // threshold: [start, end)
    uint32_t next = 1;
    for (uint32_t round = 0; round < 5000; ++round) {
        const uint32_t capacity = 1 + (round * 37) % 300, used = 1 + (round * 11) % capacity;
        uint32_t at = arena.reserve(capacity);
        while (at == SwitchHost1xArena::kNone) {
            ASSERT_NE(arena.oldestFenced(), nullptr);
            syncpoint.value = arena.oldestFenced()->threshold;
            arena.reclaim(syncpoint.reached());
            live.erase(live.begin(), live.upper_bound(syncpoint.value));
            at = arena.reserve(capacity);
        }
        ASSERT_LE(at + capacity, 1024u);
        for (const auto &[threshold, range] : live)
            ASSERT_TRUE(at + used <= range.first || at >= range.second) << "round " << round;
        ASSERT_TRUE(arena.commit(at, used, true, kSyncpt, next));
        live[next++] = {at, at + used};
        ASSERT_LE(arena.held(), 1024u);
    }
}

TEST(Host1xPush, EverySlotIsAGatherAndTheRingWrapsBeforeItsRestart)
{
    uint32_t ring[SwitchHost1xPush::kBytes / 4 + 1] = {};
    ring[SwitchHost1xPush::kBytes / 4] = SwitchHost1xPush::restart(0);
    SwitchHost1xPush push;
    for (uint32_t i = 0; i < SwitchHost1xPush::kSlots + 3; ++i)
        EXPECT_EQ(push.push(ring, i + 1, 0x80000000u + 4 * i), (i % SwitchHost1xPush::kSlots) * 8);
    EXPECT_EQ(push.put, 3u * 8);
    EXPECT_EQ(ring[0], (6u << 28) | (SwitchHost1xPush::kSlots + 1));
    EXPECT_EQ(ring[1], 0x80000000u + 4 * SwitchHost1xPush::kSlots);
    EXPECT_EQ(ring[SwitchHost1xPush::kBytes / 4], 5u << 28);
}

TEST(Host1xGatherStats, OneLineAWindowWithGathersCopiesAndTheArenaHighWaterMark)
{
    SwitchHost1xGatherStats stats = {};
    char line[200];
    EXPECT_FALSE(stats.line(1000000, line, sizeof(line)));
    stats.submitted(0, false, 300);
    stats.submitted(0, false, 1200);
    stats.submitted(0, true, 3);
    stats.submitted(3, false, 34);
    stats.waited(1);
    EXPECT_FALSE(stats.line(5999999, line, sizeof(line)));
    ASSERT_TRUE(stats.line(6000000, line, sizeof(line)));
    EXPECT_STREQ(line, "TegraHost1x: 5s NVDEC/VIC/NVENC/NVJPG gathered=2/0/0/1 copied=1/0/0/0 arena_high=5/0/0/1KB "
                       "room_waits=0/1/0/0");
    stats.submitted(2, false, 10);
    EXPECT_FALSE(stats.line(10999999, line, sizeof(line)));
    ASSERT_TRUE(stats.line(11000000, line, sizeof(line)));
    EXPECT_STREQ(line, "TegraHost1x: 5s NVDEC/VIC/NVENC/NVJPG gathered=0/0/1/0 copied=0/0/0/0 arena_high=0/0/1/0KB "
                       "room_waits=0/0/0/0");
}
}  // namespace
