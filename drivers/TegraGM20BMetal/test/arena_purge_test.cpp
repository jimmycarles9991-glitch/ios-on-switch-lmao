// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "drivers/TegraGM20BMetal/arena_purge.h"

namespace {

struct FakeArena {
    bool used = true, isVolatile = false, purged = false;
    uint32_t leases = 0;
};

void purge(std::vector<FakeArena> &arenas, bool idle = true, bool committed = false)
{
    gm20b::purgeIdleArenas(arenas, idle, committed, [](FakeArena &a) {
        a.purged = true;
    });
}

}  // namespace

TEST(GM20BArenaPurge, AnIdleVolatileHeapGivesItsPagesBack)
{
    std::vector<FakeArena> arenas(2);
    gm20b::arenaSetVolatile(arenas[0], false);
    purge(arenas);
    EXPECT_TRUE(arenas[0].purged);
    EXPECT_FALSE(arenas[1].purged);
}

TEST(GM20BArenaPurge, NotWhileTheGpuOrACommitOrAnM2vViewHoldsIt)
{
    std::vector<FakeArena> arenas(3);
    for (FakeArena &a : arenas)
        gm20b::arenaSetVolatile(a, false);
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
TEST(GM20BArenaPurge, NonVolatileBeforeThePurgeKeepsTheContentsItWasPromised)
{
    std::vector<FakeArena> arenas(1);
    gm20b::arenaSetVolatile(arenas[0], false);
    EXPECT_FALSE(gm20b::arenaSetVolatile(arenas[0], true));
    purge(arenas);
    EXPECT_FALSE(arenas[0].purged);

    gm20b::arenaSetVolatile(arenas[0], false);
    purge(arenas);
    EXPECT_TRUE(gm20b::arenaSetVolatile(arenas[0], true));
}

TEST(GM20BArenaPurge, EmptyAskedOfANonVolatileHeapPurgesItWhenIdle)
{
    FakeArena a;
    EXPECT_FALSE(gm20b::arenaPurgeable(a, true, false, false));
    EXPECT_TRUE(gm20b::arenaPurgeable(a, true, false, true));
    EXPECT_FALSE(gm20b::arenaPurgeable(a, true, true, true));
    a.purged = true;
    EXPECT_FALSE(gm20b::arenaPurgeable(a, true, false, true));
}
