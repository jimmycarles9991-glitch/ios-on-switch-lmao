// Host test of the write fence end to end: SwitchSdPick.h's pick with SwitchQueue.h's gather (or pickLocked's fallback
// merge) in the card model (sd_pick_model.h), with synchronizes beginning during the run. A synchronize must not end
// while a write submitted ahead of it is still queued or on the card, on either window, whether the synchronizes
// overlap and whether one begins over a chain already picked.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "sd_pick_model.h"

using namespace sdmodel;

namespace {

// Runs of 4 contiguous 4 KiB writes per window, each run's writes arriving out of order between other runs' and
// tier 0-2 reads, so a gather has writes to join out of submission order.
std::vector<Arrival> interleavedWrites(uint64_t seed)
{
    std::mt19937_64 random(seed);
    std::vector<Arrival> out;
    for (int run = 0; run < 60; ++run) {
        const uint64_t base = random() % 30000000 / 8 * 8;
        const bool shared = random() % 2 == 1;
        for (int i = 0; i < 4; ++i) {
            const uint64_t at = random() % (40 * kMs);
            out.push_back(
                {at, base + static_cast<uint64_t>(i) * 8, 8, static_cast<uint8_t>(random() % 3), true, shared});
        }
    }
    for (int i = 0; i < 120; ++i) {
        out.push_back({random() % (40 * kMs), random() % 30000000, 32, static_cast<uint8_t>(random() % 3), false,
                       random() % 2 == 1});
    }
    return out;
}

std::vector<Sync> syncs(uint64_t seed)
{
    std::mt19937_64 random(seed);
    std::vector<Sync> out;
    out.reserve(30);
    for (int i = 0; i < 30; ++i)
        out.push_back({random() % (40 * kMs), random() % 2 == 1});
    return out;
}

Result fenceRun(uint64_t seed, bool anywhere)
{
    Card card(Rule::kNew, interleavedWrites(seed), 4, 5 * kMs);
    card.syncAt(syncs(seed + 1000));
    if (!anywhere)
        card.mergeOnlyBehind();
    return card.run();
}

}  // namespace

TEST(SdFence, SynchronizesEndOnlyAfterEveryEarlierWrite)
{
    for (bool anywhere : {true, false}) {
        unsigned ended = 0, outOfOrder = 0;
        for (uint64_t seed = 1; seed <= 40; ++seed) {
            const Result result = fenceRun(seed, anywhere);
            EXPECT_TRUE(result.fenced) << "seed " << seed << (anywhere ? " gathered" : " merged behind");
            EXPECT_TRUE(result.ordered) << "seed " << seed;
            EXPECT_TRUE(result.writesFirst) << "seed " << seed;
            EXPECT_TRUE(result.intact) << "seed " << seed;
            EXPECT_EQ(result.unfitChains, 0u) << "seed " << seed;
            EXPECT_EQ(result.syncsEnded, 30u) << "seed " << seed;
            ended += result.syncsEnded;
            outOfOrder += result.outOfOrderWrites;
        }
        EXPECT_EQ(ended, 40u * 30u);
        if (anywhere)
            EXPECT_GT(outOfOrder, 0u);  // the gather did join writes out of order while no synchronize waited
    }
}

// W1 and W3 continue each other with W2 between them in submission order. With no synchronize waiting they leave as
// one chain, prepared behind a read; a synchronize then begins over that chain (target 3, W2 still queued) and a W4
// that continues W2 arrives. The synchronize ends only once W2 has completed too.
TEST(SdFence, ASynchronizeThatBeginsOverAChainAlreadyPicked)
{
    const std::vector<Arrival> background = {
        {0, 900000, 256, 2, false},     // on the card until 1.86 ms
        {1, 2000000, 256, 2, false},    // prepared behind it
        {2, 100, 8, 1, true},           // W1
        {3, 500, 8, 1, true},           // W2
        {4, 108, 8, 1, true},           // W3: joins W1 out of order
        {2600 * kUs, 508, 8, 1, true},  // W4, after the synchronize began
    };
    Card card(Rule::kNew, background, 0, 0);
    card.syncAt({{2500 * kUs, false}});  // W1+W3 was picked at 1.86 ms, W2 is picked at 3.72 ms
    const Result result = card.run();
    EXPECT_EQ(result.outOfOrderWrites, 1u);
    EXPECT_TRUE(result.fenced);
    EXPECT_EQ(result.syncsEnded, 1u);
}

// A synchronize's five tier-1 writes complete within a few milliseconds, but its thread takes 150 ms to run again and
// leave the fence (a CPU storm). A tier-2 bulk writer keeps writing 64 KiB every millisecond, in places that do not
// merge, and the UI takes 10 faults. Those bulk writes came after the synchronize began, so it does not count them and
// they keep tier 2; ranked with tier 0 while any synchronize waits, as before, the UI's reads queued behind them all.
TEST(SdFence, WritesQueuedAfterASynchronizeKeepTheirTier)
{
    std::vector<Arrival> background = scattered(5, 4 * kMs, 8, 1, true, 21);
    for (int i = 0; i < 100; ++i) {
        background.push_back(
            {6 * kMs + static_cast<uint64_t>(i) * kMs, 50000000 + static_cast<uint64_t>(i) * 4096, 128, 2, true});
    }
    Result results[2];
    for (bool everyWrite : {true, false}) {
        Card card(Rule::kNew, background, 10, 10 * kMs);
        card.syncAt({{5 * kMs, false}});
        card.wakeAfter(150 * kMs);
        if (everyWrite)
            card.awaitEveryWrite();
        Result &result = results[everyWrite ? 0 : 1];
        result = card.run();
        EXPECT_TRUE(result.fenced);
        EXPECT_EQ(result.syncsEnded, 1u);
    }
    std::printf("bulk writes after a synchronize: UI 10 faults %.1f -> %.1f ms, worst tier-1 write %.1f -> %.1f ms\n",
                results[0].uiTotalNs / 1e6, results[1].uiTotalNs / 1e6, results[0].worstByClass[1] / 1e6,
                results[1].worstByClass[1] / 1e6);
    EXPECT_LT(results[1].uiTotalNs * 2, results[0].uiTotalNs);
    EXPECT_LE(results[1].worstByClass[1], results[0].worstByClass[1]);
}
