// kernel/switch/sched/slots.h on the host: watched threads' slots taken without a lock, found through the filter, freed
// only from the report's snapshot, filter bits cleared only when no taken id needs them, refusals once per window.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "sched/slots.h"

namespace {

std::unique_ptr<switch_slots> Table()
{
    return std::make_unique<switch_slots>();
}

int Take(switch_slots *s, uint64_t tid)
{
    bool first = false;
    const int i = switch_slot_claim(s, tid, &first);
    if (i >= 0)
        switch_slot_publish(s, static_cast<unsigned int>(i), tid);
    return i;
}

// `n` ids from `from` on whose hash is `like`'s: they share a filter bit.
std::vector<uint64_t> SameBit(uint64_t like, size_t n, uint64_t from)
{
    std::vector<uint64_t> ids;
    for (uint64_t tid = from; ids.size() < n; tid++)
        if (switch_slot_hash(tid) == switch_slot_hash(like))
            ids.push_back(tid);
    return ids;
}

bool BitSet(const switch_slots *s, uint64_t tid)
{
    return (s->filter[switch_slot_hash(tid) >> 6] & switch_slot_bit(switch_slot_hash(tid))) != 0;
}

TEST(XnuSchedSlots, ATakenSlotIsFoundAndNoOtherIs)
{
    auto s = Table();
    const int i = Take(s.get(), 7001);
    ASSERT_GE(i, 0);
    EXPECT_EQ(switch_slot_find(s.get(), 7001), i);
    EXPECT_EQ(switch_slot_find(s.get(), 7002), -1);
}

TEST(XnuSchedSlots, AClaimIsNotFoundUntilItIsPublished)
{
    auto s = Table();
    bool first = false;
    const int i = switch_slot_claim(s.get(), 7001, &first);
    ASSERT_GE(i, 0);
    EXPECT_EQ(switch_slot_find(s.get(), 7001), -1);
    EXPECT_EQ(Take(s.get(), 7002), i + 1) << "a claimed slot is not free";
    switch_slot_publish(s.get(), static_cast<unsigned int>(i), 7001);
    EXPECT_EQ(switch_slot_find(s.get(), 7001), i);
}

TEST(XnuSchedSlots, AFullTableRefusesAThreadOnceAWindow)
{
    auto s = Table();
    for (uint64_t tid = 1; tid <= SWITCH_SLOTS; tid++)
        ASSERT_GE(Take(s.get(), tid), 0);
    bool first = false;
    EXPECT_EQ(switch_slot_claim(s.get(), 9001, &first), -1);
    EXPECT_TRUE(first);
    EXPECT_EQ(switch_slot_claim(s.get(), 9001, &first), -1);
    EXPECT_FALSE(first) << "every later dispatch is not counted again";
    switch_slot_snap snap;
    switch_slot_snapshot(s.get(), &snap);
    bool alive[SWITCH_SLOTS];
    std::fill(alive, alive + SWITCH_SLOTS, true);
    EXPECT_EQ(switch_slot_free_gone(s.get(), &snap, alive), 0u);
    EXPECT_EQ(switch_slot_claim(s.get(), 9001, &first), -1);
    EXPECT_TRUE(first) << "the report starts a new window of refusals";
}

// The review's D3: a slot taken after the report's snapshot is never freed under its thread.
TEST(XnuSchedSlots, TheReportFreesOnlySlotsItsSnapshotHeld)
{
    auto s = Table();
    const int gone = Take(s.get(), 7001);
    switch_slot_snap snap;
    switch_slot_snapshot(s.get(), &snap);
    const int late = Take(s.get(), 7002);  // taken during the walk, its thread not yet seen
    const bool alive[SWITCH_SLOTS] = {false};
    EXPECT_EQ(switch_slot_free_gone(s.get(), &snap, alive), 1u);
    EXPECT_EQ(switch_slot_find(s.get(), 7001), -1);
    EXPECT_EQ(switch_slot_find(s.get(), 7002), late);
    EXPECT_EQ(s->tids[gone], 0u);
}

TEST(XnuSchedSlots, ASeenThreadKeepsItsSlot)
{
    auto s = Table();
    const int i = Take(s.get(), 7001);
    switch_slot_snap snap;
    switch_slot_snapshot(s.get(), &snap);
    EXPECT_EQ(switch_slot_snapshot_find(&snap, 7001), i);
    EXPECT_EQ(switch_slot_snapshot_find(&snap, 7002), -1);
    bool alive[SWITCH_SLOTS] = {false};
    alive[i] = true;
    EXPECT_EQ(switch_slot_free_gone(s.get(), &snap, alive), 0u);
    EXPECT_EQ(switch_slot_find(s.get(), 7001), i);
}

// The review's D9: freeing one id clears its filter bit only when no other taken id needs that bit.
TEST(XnuSchedSlots, AFreedIdsBitStaysWhileAnotherIdNeedsIt)
{
    auto s = Table();
    const std::vector<uint64_t> pair = SameBit(7001, 2, 7001);
    Take(s.get(), pair[0]);
    const int kept = Take(s.get(), pair[1]);
    switch_slot_snap snap;
    switch_slot_snapshot(s.get(), &snap);
    bool alive[SWITCH_SLOTS] = {false};
    alive[kept] = true;
    EXPECT_EQ(switch_slot_free_gone(s.get(), &snap, alive), 1u);
    EXPECT_EQ(switch_slot_find(s.get(), pair[1]), kept);

    alive[kept] = false;
    switch_slot_snapshot(s.get(), &snap);
    EXPECT_EQ(switch_slot_free_gone(s.get(), &snap, alive), 1u);
    EXPECT_FALSE(BitSet(s.get(), pair[1])) << "no taken id needs it";
}

// A publish whose bit-set lands before the report's clear has its bit put back by the report's re-scan.
TEST(XnuSchedSlots, ATakenIdWhoseBitWasClearedGetsItBack)
{
    auto s = Table();
    const int kept = Take(s.get(), 7001);
    Take(s.get(), 7002);
    s->filter[switch_slot_hash(7001) >> 6] = 0;
    switch_slot_snap snap;
    switch_slot_snapshot(s.get(), &snap);
    bool alive[SWITCH_SLOTS] = {false};
    alive[kept] = true;
    EXPECT_EQ(switch_slot_free_gone(s.get(), &snap, alive), 1u);
    EXPECT_EQ(switch_slot_find(s.get(), 7001), kept);
}

TEST(XnuSchedSlots, FreeGoneLeavesFilterUntouchedWhenNothingIsFreed)
{
    auto s = Table();
    const int i = Take(s.get(), 7001);
    uint64_t other = 7002;
    while (switch_slot_hash(other) == switch_slot_hash(7001))
        other++;
    // Simulate a concurrent publish setting its filter bit before tids[] is seen.
    s->filter[switch_slot_hash(other) >> 6] |= switch_slot_bit(switch_slot_hash(other));
    switch_slot_snap snap;
    switch_slot_snapshot(s.get(), &snap);
    bool alive[SWITCH_SLOTS] = {false};
    alive[i] = true;
    EXPECT_EQ(switch_slot_free_gone(s.get(), &snap, alive), 0u);
    EXPECT_TRUE(BitSet(s.get(), other)) << "nothing was freed, so no filter bit may be transiently cleared";
}

TEST(XnuSchedSlots, ClaimDoesNotDuplicateAnExistingThreadWhoseFilterBitIsClear)
{
    auto s = Table();
    const int first_slot = Take(s.get(), 7001);
    const int kept = Take(s.get(), 7002);
    switch_slot_snap snap;
    switch_slot_snapshot(s.get(), &snap);
    bool alive[SWITCH_SLOTS] = {false};
    alive[kept] = true;
    ASSERT_EQ(switch_slot_free_gone(s.get(), &snap, alive), 1u);
    ASSERT_EQ(s->tids[first_slot], 0u);
    // Simulate a transient filter clear on 7002 while slot 0 is free ahead of it.
    s->filter[switch_slot_hash(7002) >> 6] &= ~switch_slot_bit(switch_slot_hash(7002));
    ASSERT_EQ(switch_slot_find(s.get(), 7002), -1);
    bool refused = true;
    EXPECT_EQ(switch_slot_claim(s.get(), 7002, &refused), -1);
    EXPECT_FALSE(refused) << "existing slot must not count as a full-table refusal";
    EXPECT_EQ(s->tids[first_slot], 0u) << "must not claim a duplicate slot";
    EXPECT_EQ(switch_slot_find(s.get(), 7002), kept) << "filter bit restored for existing slot";
}

TEST(XnuSchedSlots, AFreedSlotIsTakenAgain)
{
    auto s = Table();
    const int i = Take(s.get(), 7001);
    switch_slot_snap snap;
    switch_slot_snapshot(s.get(), &snap);
    const bool alive[SWITCH_SLOTS] = {false};
    switch_slot_free_gone(s.get(), &snap, alive);
    EXPECT_EQ(Take(s.get(), 7003), i);
    EXPECT_EQ(switch_slot_find(s.get(), 7003), i);
}

}  // namespace
