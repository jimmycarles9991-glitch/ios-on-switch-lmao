// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_arena_purge.h"

namespace {

struct FakeArena {
    bool used = true, isVolatile = false, purged = false;
    uint32_t leases = 0;
};

void purge(std::vector<FakeArena> &arenas, bool idle = true, bool committed = false)
{
    raw::purgeIdleArenas(arenas, idle, committed, [](FakeArena &a) {
        a.purged = true;
    });
}

}  // namespace

TEST(RawArenaPurge, AnIdleVolatileHeapGivesItsPagesBack)
{
    std::vector<FakeArena> arenas(2);
    raw::arenaSetVolatile(arenas[0], false);
    purge(arenas);
    EXPECT_TRUE(arenas[0].purged);
    EXPECT_FALSE(arenas[1].purged);
}

TEST(RawArenaPurge, NotWhileTheGpuOrACommitOrAnM2vViewHoldsIt)
{
    std::vector<FakeArena> arenas(3);
    for (FakeArena &a : arenas)
        raw::arenaSetVolatile(a, false);
    arenas[2].leases = 1;
    purge(arenas, false, false);
    purge(arenas, true, true);
    EXPECT_FALSE(arenas[0].purged || arenas[1].purged || arenas[2].purged);
    purge(arenas);
    EXPECT_TRUE(arenas[0].purged && arenas[1].purged);
    EXPECT_FALSE(arenas[2].purged);
}

// Core Animation makes its heap NonVolatile before it reuses what the heap holds: no purge takes the pages after that
// answer, and a purge before it makes the answer Empty
TEST(RawArenaPurge, NonVolatileBeforeThePurgeKeepsTheContentsItWasPromised)
{
    std::vector<FakeArena> arenas(1);
    raw::arenaSetVolatile(arenas[0], false);
    EXPECT_FALSE(raw::arenaSetVolatile(arenas[0], true));
    purge(arenas);
    EXPECT_FALSE(arenas[0].purged);

    raw::arenaSetVolatile(arenas[0], false);
    purge(arenas);
    EXPECT_TRUE(raw::arenaSetVolatile(arenas[0], true));
}

TEST(RawArenaPurge, EmptyAskedOfANonVolatileHeapPurgesItWhenIdle)
{
    FakeArena a;
    EXPECT_FALSE(raw::arenaPurgeable(a, true, false, false));
    EXPECT_TRUE(raw::arenaPurgeable(a, true, false, true));
    EXPECT_FALSE(raw::arenaPurgeable(a, true, true, true));
    a.purged = true;
    EXPECT_FALSE(raw::arenaPurgeable(a, true, false, true));
}
