// SPDX-License-Identifier: MIT
// zcull_planes.h and zcull_slots.h on the host: a pass's STORE_ZCULL word stays a NO_OPERATION until a pass loads its
// region or the next range will, a region carried past its job is one no other job came between, and a slot is taken
// from no job that may still load it.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../zcull_planes.h"
#include "../zcull_slots.h"

namespace {

constexpr TegraGM20BAbiZcullInfo kX1 = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};

gm20b::ZcullStored region(uint64_t zetaVa, int slot, uint32_t *store)
{
    gm20b::ZcullStored s;
    s.zetaVa = s.memoryVa = zetaVa;
    s.memoryBytes = 0x1000;
    s.va = 0x900000 + uint64_t(slot) * 0x10000;
    s.slot = slot;
    s.store = store;
    s.valid = true;
    return s;
}

TEST(ZcullPlanes, AStoreIsANoOperationUntilAPassLoadsTheRegion)
{
    uint32_t word = gm20b::kZcullUnstoredWord;
    gm20b::ZcullPlanes planes;
    planes.stored(region(0x100000, 0, &word));
    EXPECT_EQ(word, gm20b::kZcullUnstoredWord);
    EXPECT_FALSE(planes.loaded(0x100000)) << "stored by this job";
    EXPECT_EQ(word, gm20b::kZcullStoreWord);
    EXPECT_EQ(planes.stores(), 1u);
    word = gm20b::kZcullUnstoredWord;
    planes.loaded(0x100000);
    EXPECT_EQ(word, gm20b::kZcullUnstoredWord) << "decided once: the word is not written again";
    EXPECT_EQ(planes.stores(), 1u);
}

// A pass that clears and stores the depth again replaces its region; the earlier one no pass loaded stays unstored
TEST(ZcullPlanes, ARegionNoPassLoadedIsLeftUnstored)
{
    uint32_t first = gm20b::kZcullUnstoredWord, second = gm20b::kZcullUnstoredWord;
    gm20b::ZcullPlanes planes;
    planes.stored(region(0x100000, 1, &first));
    EXPECT_EQ(planes.slotOf(0x100000), 1) << "the depth's next store takes its slot again";
    planes.stored(region(0x100000, 1, &second));
    EXPECT_EQ(planes.regions().size(), 1u);
    EXPECT_EQ(planes.unstored(), 1u);
    planes.loaded(0x100000);
    EXPECT_EQ(first, gm20b::kZcullUnstoredWord);
    EXPECT_EQ(second, gm20b::kZcullStoreWord);
}

// At the commit, a region a later range loads is stored and kept, carried; the others are dropped, their slots given back
TEST(ZcullPlanes, TheCommitStoresWhatTheNextRangeLoadsAndDropsTheRest)
{
    uint32_t words[3] = {gm20b::kZcullUnstoredWord, gm20b::kZcullUnstoredWord, gm20b::kZcullUnstoredWord};
    gm20b::ZcullPlanes planes;
    planes.stored(region(0x100000, 0, &words[0]));
    planes.stored(region(0x200000, 1, &words[1]));
    planes.stored(region(0x300000, 2, &words[2]));
    planes.loaded(0x100000);
    planes.wrote(0x300000, 4);
    std::vector<int> released;
    planes.settle(0b110, [&](int slot) {
        released.push_back(slot);
    });
    EXPECT_EQ(words[0], gm20b::kZcullStoreWord) << "loaded in its job";
    EXPECT_EQ(words[1], gm20b::kZcullStoreWord) << "kept for the next range";
    EXPECT_EQ(words[2], gm20b::kZcullUnstoredWord) << "stale: kept for nothing";
    EXPECT_EQ(released, (std::vector<int>{0, 2}));
    ASSERT_EQ(planes.regions().size(), 1u);
    EXPECT_EQ(planes.regions()[0].zetaVa, 0x200000u);
    EXPECT_TRUE(planes.regions()[0].carried);
    EXPECT_EQ(planes.regions()[0].store, nullptr);
    EXPECT_EQ(planes.stores(), 2u);
    EXPECT_EQ(planes.unstored(), 1u);

    gm20b::ZcullPlanes next;
    next.carryFrom(planes);
    EXPECT_TRUE(planes.empty());
    EXPECT_EQ(next.stores(), 0u) << "counted by the job that stored them";
    EXPECT_TRUE(next.loaded(0x200000)) << "carried";
    EXPECT_EQ(next.stores(), 0u);
}

// A slot for a new region comes from a stale region first, then from the oldest region no pass loaded, never from one a
// pass loaded
TEST(ZcullPlanes, ASlotIsReclaimedOnlyFromARegionNoPassWillLoad)
{
    uint32_t words[3] = {gm20b::kZcullUnstoredWord, gm20b::kZcullUnstoredWord, gm20b::kZcullUnstoredWord};
    gm20b::ZcullPlanes planes;
    planes.stored(region(0x100000, 0, &words[0]));
    planes.stored(region(0x200000, 1, &words[1]));
    planes.stored(region(0x300000, 2, &words[2]));
    planes.loaded(0x100000);
    planes.wrote(0x300000, 4);
    EXPECT_EQ(planes.reclaim(), 2) << "stale";
    EXPECT_EQ(planes.reclaim(), 1) << "unloaded";
    EXPECT_EQ(planes.reclaim(), -1) << "loaded: a later pass may load it again";
    EXPECT_EQ(planes.held(0x100000).zetaVa, 0x100000u);
    EXPECT_EQ(words[1], gm20b::kZcullUnstoredWord);
}

TEST(ZcullSlots, ASlotIsTakenFromNoJobThatMayStillLoadIt)
{
    EXPECT_EQ(gm20b::ZcullSlots::slotBytes(kX1), 0x10400u) << "2048 aliquots of 32 bytes and 224 of headers, aligned";
    gm20b::ZcullSlots slots;
    uint64_t nextVa = 0x800000;
    int made = 0;
    const auto make = [&](gm20b::Bo &bo, uint64_t bytes) {
        EXPECT_EQ(bytes, 0x10400u);
        bo.va = nextVa;
        nextVa += bytes;
        made++;
        return true;
    };
    EXPECT_EQ(slots.take(1, 0, 0x10400, make), 0);
    EXPECT_EQ(slots.take(1, 0, 0x10400, make), 1) << "its own slot is not free to it";
    EXPECT_EQ(slots.take(2, 1, 0x10400, make), 2) << "a parked carry's slots are not free";
    EXPECT_EQ(slots.take(3, 0, 0x10400, make), 0) << "a job no carry names holds nothing past its own";
    EXPECT_EQ(made, 3);
    EXPECT_EQ(slots.va(1), 0x810400u);
    slots.take(3, 0, 0x10400, make);
    slots.take(3, 0, 0x10400, make);
    slots.take(3, 0, 0x10400, make);
    EXPECT_EQ(made, 4);
    EXPECT_EQ(slots.take(3, 0, 0x10400, make), -1) << "four, all its own";
    slots.release(2);
    EXPECT_EQ(slots.take(3, 0, 0x10400, make), 2);
    gm20b::ZcullSlots none;
    EXPECT_EQ(none.take(1, 0, 0x10400,
                        [](gm20b::Bo &, uint64_t) {
                            return false;
                        }),
              -1)
        << "no memory: no store, never a wait";
}

// Between jobs a slot is made ahead only where the next job could take none: never before a depth first needed one,
// not while a made slot is free to the next job, and once where every made slot is a live carry's
TEST(ZcullSlots, ASlotIsMadeAheadOnlyWhereTheNextJobCouldTakeNone)
{
    gm20b::ZcullSlots slots;
    int made = 0;
    const auto make = [&](gm20b::Bo &bo, uint64_t) {
        bo.va = 0x800000 + 0x10400 * uint64_t(made++);
        return true;
    };
    slots.makeAhead(0, 0x10400, make);
    EXPECT_EQ(made, 0) << "a process that drew no depth keeps no slot";
    ASSERT_EQ(slots.take(1, 0, 0x10400, make), 0);
    slots.makeAhead(0, 0x10400, make);
    EXPECT_EQ(made, 1) << "slot 0 is free to a job no carry names";
    slots.makeAhead(1, 0x10400, make);
    EXPECT_EQ(made, 2) << "slot 0 is the carry's";
    EXPECT_EQ(slots.owner(1), 0u);
    EXPECT_EQ(slots.take(2, 1, 0x10400, make), 1) << "the next job takes the one made ahead";
    EXPECT_EQ(made, 2);
    slots.makeAhead(1, 0x10400, make);
    EXPECT_EQ(made, 2) << "slot 1 is free to the next job";
}

TEST(ZcullCarry, ACarryLivesWhileNoOtherJobWasSubmittedOnItsChannel)
{
    gm20b::ZcullCarry carry;
    EXPECT_FALSE(carry.live(1, 0)) << "parked for nothing";
    int cb = 0;
    carry.to = &cb;
    carry.generation = 3;
    carry.submitted = 40;
    EXPECT_TRUE(carry.live(3, 40));
    EXPECT_FALSE(carry.live(3, 41)) << "another job was submitted";
    EXPECT_FALSE(carry.live(4, 40)) << "a channel made again";
}

}  // namespace
