// Host test of drivers/NativeSwitchBlock/SwitchSdPick.h: the SD worker's choice of chain, run in the card model
// (sd_pick_model.h) against the rule it replaced (r6-lows: a better-class read passes only a head of another class, and
// only while the head is under 100 ms old).
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "sd_pick_model.h"

using namespace sdmodel;

namespace {

Result run(Rule rule, const std::vector<Arrival> &background, bool syncWaiting = false, int faults = 10)
{
    Card card(rule, background, faults, 2 * kMs, syncWaiting);
    const Result result = card.run();
    EXPECT_TRUE(result.ordered);
    EXPECT_TRUE(result.writesFirst);
    EXPECT_TRUE(result.intact);
    EXPECT_EQ(result.unfitChains, 0u);
    return result;
}

}  // namespace

// Dump 5: 73 APFS metadata writes at tier 0 (passive) behind which SpringBoard's reads queued. Under the old rule a
// tier-0 write at the head stops every read; now the reads go first, and the writes all still finish.
TEST(SdPick, MetadataWritesNoLongerHoldTheUi)
{
    const auto background = join(scattered(73, 5 * kMs, 8, 0, true, 1), scattered(30, 20 * kMs, 64, 2, false, 2));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 5: 73 tier-0 metadata writes", before, after);
    EXPECT_LT(after.uiTotalNs * 2, before.uiTotalNs);
    EXPECT_LT(after.uiWorstNs, 5 * kMs);
    EXPECT_LT(after.lowerWorstNs, 1000 * kMs);
}

// The same writes as IOSCHED marks them (metadata writes at tier 1, passive: spec_strategy). They now also pass the
// older tier-2 reads, which costs the UI a command or so at most.
TEST(SdPick, MetadataWritesAtTierOne)
{
    const auto background = join(scattered(73, 5 * kMs, 8, 1, true, 1), scattered(30, 20 * kMs, 64, 2, false, 2));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 5, IOSCHED tiers", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs + kMs);
    EXPECT_LT(after.uiWorstNs, 5 * kMs);
}

// Dump 4 once the pack warm reads at tier 3 (the state lane's fix). The old rule already let tier 0 pass it. What is
// left is a 512 KiB read prepared behind the UI's; the software throttle XNU keeps for this device holds tier-3 reads
// back while tier 0 is busy.
TEST(SdPick, SingleLargeBackgroundReadsAreNoWorse)
{
    const auto background = join(stream(16, 10 * kMs, 3), scattered(21, 50 * kMs, 64, 2, false, 3));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 4, pack warm at tier 3", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs);
}

// Dump 4 as it ran: the warm's 512 KiB reads at tier 0, the UI's own class. No class rule can separate them, which
// is why the warm's tier has to be fixed at its source; this rule must not make it worse.
TEST(SdPick, SameClassBulkReadsStayFirstComeFirstServed)
{
    const auto background = join(stream(16, 10 * kMs, 0), scattered(21, 50 * kMs, 64, 2, false, 3));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 4 as it ran, pack warm at tier 0", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs + kMs);
}

// Dump 6: 148 other processes' tier-0 page-ins and 75 tier-2 ones. The tier-2 part no longer delays the UI; the
// tier-0 part is the UI's own class and keeps its order.
TEST(SdPick, OtherTierZeroPageInsKeepTheirOrder)
{
    const auto background = join(scattered(148, 30 * kMs, 32, 0, false, 5), scattered(75, 30 * kMs, 64, 2, false, 6));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 6: 148 tier-0 + 75 tier-2 page-ins", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs + kMs);
}

// Tier-0 reads arriving faster than the card serves them (every 0.5 ms, 16 KiB each) never let the queue empty. A
// tier-3 read still finishes: once the head is kSwitchSdStarveNs old it goes first after kSwitchSdShareNs or
// kSwitchSdShareChains passes.
TEST(SdPick, BackgroundIsNeverStarved)
{
    std::vector<Arrival> background = scattered(20, kMs, 64, 3, false, 7);
    background.reserve(background.size() + 4000);
    for (int i = 0; i < 4000; ++i)
        background.push_back(
            {static_cast<uint64_t>(i) * 500 * kUs, 30000000 + static_cast<uint64_t>(i) * 977, 32, 0, false});
    const Result before = run(Rule::kOld, background, false, 0);
    const Result after = run(Rule::kNew, background, false, 0);
    std::printf("starvation: worst tier-3 read %.1f -> %.1f ms, worst tier-0 read %.1f -> %.1f ms\n",
                before.worstByClass[3] / 1e6, after.worstByClass[3] / 1e6, before.worstByClass[0] / 1e6,
                after.worstByClass[0] / 1e6);
    EXPECT_GT(after.worstByClass[3], 0u);
    EXPECT_LT(after.worstByClass[3], 400 * kMs);  // 224 ms; the 50 ms share alone, without 8 passes, gave 1,010 ms
}

// The boot replay keeps up to 1.5 MiB of 512 KiB reads queued for as long as it runs (SWITCH_PREFETCH_INFLIGHT_LIMIT:
// three queued at once, then one as each completes). In tier 0's class the UI's page-ins wait behind all of them; one
// class lower they pass the queued ones and wait for the chain on the card and the one prepared behind it. Runs of
// different files (the replay list's usual order) stay 512 KiB chains; one file's consecutive runs merge into 1 MiB
// chains, which the pipeline holds either way.
TEST(SdPick, BootReplayRanksBelowTierZeroDemand)
{
    const auto replay = [](uint8_t klass, uint64_t stride) {
        std::vector<Arrival> out;
        for (int i = 0; i < 300; ++i)
            out.push_back({i < 3 ? 0 : static_cast<uint64_t>(i - 2) * 5700 * kUs,
                           80000000 + static_cast<uint64_t>(i) * stride, 1024, klass, false, false, true});
        return out;
    };
    EXPECT_EQ(switchSdClassOf(0, false), 0u);
    EXPECT_EQ(switchSdClassOf(2, true), 2u);
    const Result before = run(Rule::kNew, replay(0, 8192));
    const Result after = run(Rule::kNew, replay(switchSdClassOf(0, true), 8192));
    report("boot replay of separate files, below tier 0", before, after);
    EXPECT_LT(after.uiTotalNs * 3, before.uiTotalNs * 2);  // 439.1 -> 239.6 ms
    EXPECT_LT(after.uiWorstNs, 15 * kMs);                  // 28.9 -> 11.8 ms
    EXPECT_LE(after.endNs, before.endNs + 20 * kMs);       // the replay's last read: 1,723.6 ms both
    const Result mergedBefore = run(Rule::kNew, replay(0, 1024));
    const Result mergedAfter = run(Rule::kNew, replay(switchSdClassOf(0, true), 1024));
    report("boot replay of one file, below tier 0", mergedBefore, mergedAfter);
    EXPECT_LE(mergedAfter.uiTotalNs, mergedBefore.uiTotalNs);  // 342.6 -> 336.9 ms
}

// Other processes' tier-0 page-ins keep the card about 87% busy (16 KiB every 0.85 ms) while the replay reads 512 KiB
// every 40 ms (25.3 ms worst in class 0). One class lower,
// a replay read waits for the share turn, and a UI thread sleeping on one of its busy pages waits with it: XNU's
// busy-page raise is not yet seen reaching the card. After kSwitchSdReplayAgeNs it goes back to class 0, so it waits
// at most about that much longer than it did in class 0.
TEST(SdPick, AReplayReadWaitsNoLongerThanItsAgeBehindTierZero)
{
    const auto load = [](uint8_t klass, bool replay) {
        std::vector<Arrival> out;
        for (int i = 0; i < 12; ++i)
            out.push_back({static_cast<uint64_t>(i) * 40 * kMs, 80000000 + static_cast<uint64_t>(i) * 8192, 1024, klass,
                           false, false, replay});
        for (int i = 0; i < 560; ++i)
            out.push_back(
                {static_cast<uint64_t>(i) * 850 * kUs, 30000000 + static_cast<uint64_t>(i) * 977, 32, 0, false});
        return out;
    };
    const Result tierZero = run(Rule::kNew, load(0, true));
    const Result aged = run(Rule::kNew, load(switchSdClassOf(0, true), true));
    const Result unaged = run(Rule::kNew, load(switchSdClassOf(0, true), false));
    std::printf("replay beside a near-full tier-0 load: worst replay read %.1f ms in class 0, %.1f ms one class lower "
                "with the "
                "age, %.1f ms without\n",
                tierZero.replayWorstNs / 1e6, aged.replayWorstNs / 1e6, unaged.worstByClass[1] / 1e6);
    EXPECT_LE(aged.replayWorstNs, tierZero.replayWorstNs + kSwitchSdReplayAgeNs + 12 * kMs);
    EXPECT_GT(unaged.worstByClass[1], aged.replayWorstNs + kSwitchSdReplayAgeNs);
}

// A synchronize waiting for writes makes them rank with tier-0 reads: its writes are not passed, at tier 0 or at
// IOSCHED's tier 1.
TEST(SdPick, WritesASynchronizeWaitsForAreNotPassed)
{
    for (uint8_t klass : {uint8_t{0}, uint8_t{1}}) {
        const auto background = scattered(40, kMs, 8, klass, true, 8);
        const Result waiting = run(Rule::kNew, background, true);
        const Result free = run(Rule::kNew, background, false);
        std::printf("tier-%u writes with a synchronize waiting: worst write %.1f ms, without %.1f ms\n", klass,
                    waiting.lowerWorstNs / 1e6, free.lowerWorstNs / 1e6);
        EXPECT_LT(waiting.lowerWorstNs, free.lowerWorstNs);
    }
}

// A synchronize waits for writes queued behind older tier-2 reads while tier-0 reads keep arriving: the oldest write
// passes the reads, so the synchronize is not held to the one-in-eight share of a passed-over head.
TEST(SdPick, AwaitedWritesPassOlderLowerReads)
{
    std::vector<Arrival> background = scattered(60, kMs, 128, 2, false, 10);
    const auto writes = scattered(20, kMs, 8, 1, true, 11);
    background.insert(background.end(), writes.begin(), writes.end());
    for (Arrival &write : background) {
        if (write.write)
            write.at += kMs;
    }
    const Result before = run(Rule::kOld, background, true, 40);
    const Result after = run(Rule::kNew, background, true, 40);
    std::printf("awaited writes behind tier-2 reads: worst write %.1f -> %.1f ms, UI %.1f -> %.1f ms\n",
                before.worstByClass[1] / 1e6, after.worstByClass[1] / 1e6, before.uiTotalNs / 1e6,
                after.uiTotalNs / 1e6);
    EXPECT_LE(after.worstByClass[1], before.worstByClass[1]);
    EXPECT_LT(after.worstByClass[1], 25 * kMs);  // 20.9 ms; held to the share turn, as before c91064983, 100 ms
}

// APFS writing steadily (200 scattered 4 KiB writes a second, tier 1) beside tier-0 reads that keep the card ~75% busy
// on their own: the writes keep up and the queue stays far below the request pool (1024).
TEST(SdPick, SteadyWritesKeepUpUnderATierZeroFlood)
{
    std::vector<Arrival> background = scattered(400, 2000 * kMs, 8, 1, true, 9);
    background.reserve(background.size() + 2000);
    for (int i = 0; i < 2000; ++i)
        background.push_back({static_cast<uint64_t>(i) * kMs, 30000000 + static_cast<uint64_t>(i) * 977, 32, 0, false});
    const Result before = run(Rule::kOld, background, false, 0);
    const Result after = run(Rule::kNew, background, false, 0);
    std::printf("steady writes under a flood: worst write %.1f -> %.1f ms, worst tier-0 read %.1f -> %.1f ms, deepest "
                "queue %zu -> %zu\n",
                before.worstByClass[1] / 1e6, after.worstByClass[1] / 1e6, before.worstByClass[0] / 1e6,
                after.worstByClass[0] / 1e6, before.maxDepth, after.maxDepth);
    EXPECT_LT(after.maxDepth, 256u);
    EXPECT_LT(after.worstByClass[1], 1000 * kMs);
}

TEST(SdPick, RankAndPass)
{
    std::vector<Request> q(4);
    q[0] = {&q[1], 100, 8, 0, true, false, 0, 0, false, 0};   // tier-0 write, head
    q[1] = {&q[2], 300, 8, 0, false, false, 0, 1, false, 0};  // passes
    q[2] = {&q[3], 100, 8, 0, false, false, 0, 2, false, 0};  // overlaps the head
    q[3] = {nullptr, 500, 8, 2, false, false, 0, 3, false, 0};
    SwitchSdPickState state = {};
    SwitchSdPicked<Request> picked = switchSdPick(q.data(), kNoSync, kMs, &state);
    EXPECT_EQ(picked.first, &q[1]);
    EXPECT_EQ(picked.prev, q.data());
    EXPECT_TRUE(picked.passed);
    picked = switchSdPick(q.data(), kSyncAfterAll, kMs, &state);
    EXPECT_EQ(picked.first, q.data());
    picked = switchSdPick(q.data(), kNoSync, kSwitchSdStarveNs, &state);
    EXPECT_EQ(picked.first, q.data());
    EXPECT_TRUE(picked.starved);
    picked = switchSdPick(q.data(), kNoSync, kSwitchSdStarveNs + 1, &state);
    EXPECT_EQ(picked.first, &q[1]);
}

// RankAndPass's earlier layout: a tier-0 read inside the tier-0 head write is older than the tier-0 read that may
// pass, so it lends rank 0 to the head and the head goes first (review N2: pinned beside the new layout).
TEST(SdPick, AnOlderBlockedReadLendsToTheHeadBeforeAYoungerOneMayPass)
{
    std::vector<Request> q(4);
    q[0] = {&q[1], 100, 8, 0, true, false, 0, 0, false, 0};   // tier-0 write, head
    q[1] = {&q[2], 100, 8, 0, false, false, 0, 1, false, 0};  // overlaps it: lends rank 0
    q[2] = {&q[3], 300, 8, 0, false, false, 0, 2, false, 0};  // could pass, but is younger
    q[3] = {nullptr, 500, 8, 2, false, false, 0, 3, false, 0};
    SwitchSdPickState state = {};
    const SwitchSdPicked<Request> picked = switchSdPick(q.data(), kNoSync, kMs, &state);
    EXPECT_EQ(picked.first, q.data());
    EXPECT_FALSE(picked.passed);
}

// A better request that may not pass an older one lends its rank to it, however its rank came to be better: queued at
// tier 0, raised by doSetPriority, or counted by a synchronize that began.
TEST(SdPick, ABlockedRequestLendsItsRankToWhatHoldsItBack)
{
    std::vector<Request> q(5);
    q[0] = {&q[1], 900, 8, 1, false, false, 0, 0, false, 0};    // tier-1 read, head
    q[1] = {&q[2], 100, 64, 3, true, false, 0, 1, false, 0};    // tier-3 write
    q[2] = {&q[3], 2000, 8, 1, false, false, 0, 2, false, 0};   // tier-1 read
    q[3] = {&q[4], 3000, 8, 1, false, false, 0, 3, false, 0};   // tier-1 read
    q[4] = {nullptr, 160, 8, 0, false, false, 0, 4, false, 0};  // tier-0 page-in over the write's end
    SwitchSdPickState state = {};
    EXPECT_EQ(switchSdPick(q.data(), kNoSync, kMs, &state).first, &q[1]);

    q[4].klass = 2;
    EXPECT_EQ(switchSdPick(q.data(), kNoSync, kMs, &state).first, q.data());
    uint32_t reads[4] = {0, 3, 1, 0};
    uint32_t replay = 0;
    EXPECT_EQ(switchSdPromote(q.data(), 164, 4, 0, reads, replay), 1u);  // the page-in's part past the write
    EXPECT_EQ(switchSdPick(q.data(), kNoSync, kMs, &state).first, &q[1]);

    std::vector<Request> w(3);
    w[0] = {&w[1], 900, 8, 1, false, false, 5, 0, false, 0};   // tier-1 read, head
    w[1] = {&w[2], 100, 64, 2, false, false, 6, 1, false, 0};  // tier-2 read
    w[2] = {nullptr, 104, 8, 2, true, false, 7, 2, false, 0};  // tier-2 write inside it
    EXPECT_EQ(switchSdPick(w.data(), kNoSync, kMs, &state).first, w.data());
    const uint64_t rootWaits[2] = {10, 0};
    const SwitchSdPicked<Request> picked = switchSdPick(w.data(), rootWaits, kMs, &state);
    EXPECT_EQ(picked.first, &w[1]);
    EXPECT_EQ(picked.prev, w.data());
}

// The pick on a queue 1,024 deep: a tier-0 page-in at the tail inside the newest of 1,023 tier-3 writes lends its rank
// to the oldest write, which the newest may not pass.
TEST(SdPick, ADeepQueue)
{
    std::vector<Request> q(1024);
    for (size_t i = 0; i + 1 < q.size(); ++i)
        q[i] = {&q[i + 1], 1000 + i * 64, 64, 3, true, false, 0, static_cast<int>(i), false, 0};
    q.back() = {nullptr, 1000 + (q.size() - 2) * 64, 8, 0, false, false, 0, 1023, false, 0};
    SwitchSdPickState state = {};
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
        EXPECT_EQ(switchSdPick(q.data(), kNoSync, kMs, &state).first, q.data());
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
    std::printf("1,024-deep pick: %.1f us each (host)\n", us / 1000);
}

// Writes and reads pass by the same rank: a tier-0 write goes ahead of an older tier-2 read even when no read could
// pass that head, but never past an older write or a request it overlaps.
TEST(SdPick, AWritePassesByRankAlone)
{
    std::vector<Request> q(4);
    q[0] = {&q[1], 100, 8, 2, false, false, 0, 0, false, 0};  // tier-2 read, head
    q[1] = {&q[2], 200, 8, 2, false, false, 0, 1, false, 0};
    q[2] = {&q[3], 300, 8, 0, true, false, 0, 2, false, 0};  // tier-0 write: passes both reads
    q[3] = {nullptr, 400, 8, 0, true, false, 0, 3, false, 0};
    SwitchSdPickState state = {};
    EXPECT_EQ(switchSdPick(q.data(), kNoSync, kMs, &state).first, &q[2]);
    q[2].lba = 104;  // now overlaps the head read: it waits, and the write behind it may not pass it
    EXPECT_EQ(switchSdPick(q.data(), kNoSync, kMs, &state).first, q.data());
}

// Only the writes a waiting synchronize counts rank with tier 0: those of its window queued before it began.
TEST(SdPick, OnlyCountedWritesRankWithTierZero)
{
    Request counted = {nullptr, 0, 8, 2, true, false, 10, 0, false, 0};
    Request later = {nullptr, 0, 8, 2, true, false, 30, 1, false, 0};
    Request otherWindow = {nullptr, 0, 8, 2, true, false, 10, 2, false, 0, true};
    Request read = {nullptr, 0, 8, 2, false, false, 10, 3, false, 0};
    const uint64_t rootWaits[2] = {20, 0};
    EXPECT_EQ(switchSdRank(&counted, rootWaits), 0u);
    EXPECT_EQ(switchSdRank(&later, rootWaits), 5u);
    EXPECT_EQ(switchSdRank(&otherWindow, rootWaits), 5u);
    EXPECT_EQ(switchSdRank(&read, rootWaits), 4u);
    EXPECT_EQ(switchSdRank(&counted, kNoSync), 5u);
}

// The ager moves replay reads only: a replay-flagged write keeps its class and the read counts stay in step.
TEST(SdPick, OnlyReplayReadsAge)
{
    std::vector<Request> q(3);
    for (size_t i = 0; i < q.size(); ++i) {
        q[i] = {};
        q[i].next = i + 1 < q.size() ? &q[i + 1] : nullptr;
        q[i].lba = 1000 * i;
        q[i].sectors = 8;
        q[i].klass = kSwitchSdReplayClass;
        q[i].replay = true;
    }
    q[1].write = true;
    q[2].queuedNs = kSwitchSdReplayAgeNs;
    uint32_t reads[kSwitchSdPickClasses] = {0, 2, 0, 0};
    uint32_t replay = 2;
    EXPECT_EQ(switchSdAgeReplay(&q[0], kSwitchSdReplayAgeNs, reads, replay), 1u);
    EXPECT_EQ(q[0].klass, 0u);
    EXPECT_EQ(q[1].klass, kSwitchSdReplayClass);
    EXPECT_EQ(q[2].klass, kSwitchSdReplayClass);
    EXPECT_EQ(reads[0], 1u);
    EXPECT_EQ(reads[1], 1u);
    EXPECT_EQ(replay, 1u);
}

// After the boot, class 1 still holds tier-1 and cryptex reads, and queuedReads[1] counts them. The ager's walk is
// gated on the replay count alone: the queued read here is flagged so that a walk would show by moving it.
TEST(SdPick, TheAgerWalksOnlyWhileAReplayReadIsCounted)
{
    Request queued = {};
    queued.klass = kSwitchSdReplayClass;
    queued.replay = true;
    uint32_t reads[kSwitchSdPickClasses] = {0, 1, 0, 0};
    uint32_t replay = 0;
    EXPECT_EQ(switchSdAgeReplay(&queued, 10 * kSwitchSdReplayAgeNs, reads, replay), 0u);
    EXPECT_EQ(queued.klass, kSwitchSdReplayClass);
}

// The walk ends once the count is spent: the second read here is ageable but uncounted, so a walk past the first would
// move it and take the count below zero.
TEST(SdPick, TheAgerStopsOnceItsCountIsSpent)
{
    std::vector<Request> q(2);
    for (size_t i = 0; i < q.size(); ++i) {
        q[i] = {};
        q[i].next = i + 1 < q.size() ? &q[i + 1] : nullptr;
        q[i].lba = 100 * i;
        q[i].sectors = 8;
        q[i].klass = kSwitchSdReplayClass;
        q[i].replay = true;
    }
    uint32_t reads[kSwitchSdPickClasses] = {0, 2, 0, 0};
    uint32_t replay = 1;
    EXPECT_EQ(switchSdAgeReplay(q.data(), 10 * kSwitchSdReplayAgeNs, reads, replay), 1u);
    EXPECT_EQ(replay, 0u);
    EXPECT_EQ(q[1].klass, kSwitchSdReplayClass);
}

// A raise to class 0 takes a replay read out of the ager's count; a raise to class 1 leaves it there.
TEST(SdPick, PromotingAReplayReadToClassZeroLeavesTheAgerNothingToCount)
{
    std::vector<Request> q(2);
    for (size_t i = 0; i < q.size(); ++i) {
        q[i] = {};
        q[i].next = i + 1 < q.size() ? &q[i + 1] : nullptr;
        q[i].lba = 100 * i;
        q[i].sectors = 8;
        q[i].replay = true;
    }
    q[0].klass = 2;
    q[1].klass = kSwitchSdReplayClass;
    uint32_t reads[kSwitchSdPickClasses] = {0, 1, 1, 0};
    uint32_t replay = 1;
    EXPECT_EQ(switchSdPromote(q.data(), 0, 8, 1, reads, replay), 1u);
    EXPECT_EQ(replay, 2u);
    EXPECT_EQ(switchSdPromote(q.data(), 0, 200, 0, reads, replay), 2u);
    EXPECT_EQ(replay, 0u);
    EXPECT_EQ(reads[0], 2u);
}

// The age bounds the replay class only: a replay read asked at a lower priority is not lifted to class 0 by waiting, and
// a promote into the replay class makes it ageable.
TEST(SdPick, OnlyTheReplayClassAgesAndAPromoteIntoItCounts)
{
    std::vector<Request> q(2);
    for (size_t i = 0; i < q.size(); ++i) {
        q[i] = {};
        q[i].next = i + 1 < q.size() ? &q[i + 1] : nullptr;
        q[i].lba = 100 * i;
        q[i].sectors = 8;
        q[i].replay = true;
    }
    q[0].klass = 2;
    q[1].klass = 3;
    uint32_t reads[kSwitchSdPickClasses] = {0, 0, 1, 1};
    uint32_t replay = 0;
    EXPECT_EQ(switchSdPromote(q.data(), 0, 8, kSwitchSdReplayClass, reads, replay), 1u);
    EXPECT_EQ(replay, 1u);
    EXPECT_EQ(switchSdAgeReplay(q.data(), 10 * kSwitchSdReplayAgeNs, reads, replay), 1u);
    EXPECT_EQ(q[0].klass, 0u);
    EXPECT_EQ(q[1].klass, 3u);
    EXPECT_EQ(replay, 0u);
    EXPECT_EQ(reads[0], 1u);
    EXPECT_EQ(reads[3], 1u);
}

TEST(SdPick, PromoteRaisesOnly)
{
    std::vector<Request> q(3);
    q[0] = {&q[1], 0, 64, 3, false, false, 0, 0, false, 0};
    q[1] = {&q[2], 64, 64, 1, true, false, 0, 1, false, 0};
    q[2] = {nullptr, 200, 8, 0, false, false, 0, 2, false, 0};
    uint32_t reads[4] = {1, 0, 0, 1};
    uint32_t replay = 0;
    EXPECT_EQ(switchSdPromote(q.data(), 32, 300, 1, reads, replay), 1u);
    EXPECT_EQ(q[0].klass, 1);
    EXPECT_EQ(q[1].klass, 1);
    EXPECT_EQ(q[2].klass, 0);
    EXPECT_EQ(reads[0], 1u);
    EXPECT_EQ(reads[1], 1u);
    EXPECT_EQ(reads[3], 0u);
    EXPECT_EQ(switchSdPromote(q.data(), 0, 1000, 0, reads, replay), 2u);
    EXPECT_EQ(reads[0], 2u);
    EXPECT_EQ(reads[1], 0u);
}

// An empty extent raises nothing, not even a request that straddles its start.
TEST(SdPick, AnEmptyExtentRaisesNothing)
{
    std::vector<Request> q(1);
    q[0] = {nullptr, 100, 64, 3, false, false, 0, 0, false, 0};
    uint32_t reads[4] = {0, 0, 0, 1};
    uint32_t replay = 0;
    EXPECT_EQ(switchSdPromote(q.data(), 120, 0, 0, reads, replay), 0u);
    EXPECT_EQ(q[0].klass, 3);
}
