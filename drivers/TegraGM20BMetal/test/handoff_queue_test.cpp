// SPDX-License-Identifier: MIT
#include "../completion_source.h"
#include "../handoff_queue.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

struct Item {
    uint64_t index = 0;
    std::vector<uint64_t> payload;
    bool quiet = false;
};

size_t length(const gm20b::HandoffQueue<Item> &queue)
{
    size_t items = 0;
    for (auto it = queue.begin(); it != queue.end(); ++it)
        ++items;
    return items;
}

TEST(HandoffQueue, ATakerGetsEveryItemInOrderWhileTheGiverRunsAhead)
{
    gm20b::HandoffQueue<Item> queue;
    constexpr uint64_t kItems = 200000;
    std::thread giver([&] {
        for (uint64_t i = 0; i < kItems; ++i)
            queue.push({i, {i, i + 1}});
    });
    uint64_t wrong = 0;
    for (uint64_t i = 0; i < kItems; ++i) {
        Item item = std::move(queue.front());
        wrong += item.index != i || item.payload.size() != 2 || item.payload[1] != i + 1;
        queue.pop();
    }
    giver.join();
    EXPECT_EQ(wrong, 0u);
    EXPECT_EQ(queue.size(), 0u);
}

// As the submit worker takes its schedule from every recording thread and the completion thread: each giver's items in
// its order and none lost or doubled while their spare nodes go round between givers and the taker
TEST(HandoffQueue, SeveralGiversEachKeepTheirOrderAcrossSpareNodes)
{
    gm20b::HandoffQueue<Item> queue;
    constexpr uint64_t kGivers = 4, kItems = 50000;
    std::vector<std::thread> givers;
    for (uint64_t g = 0; g < kGivers; ++g)
        givers.emplace_back([&queue, g] {
            for (uint64_t i = 0; i < kItems; ++i)
                queue.push({g << 32 | i, {i}}, i % 3 == 0);
        });
    std::vector<uint64_t> next(kGivers, 0);
    uint64_t wrong = 0;
    for (uint64_t taken = 0; taken < kGivers * kItems; ++taken) {
        const Item &item = queue.front();
        const uint64_t g = item.index >> 32, i = item.index & 0xffffffff;
        wrong += g >= kGivers || i != next[g] || item.payload.size() != 1 || item.payload[0] != i;
        if (g < kGivers)
            next[g] = i + 1;
        queue.pop();
    }
    for (std::thread &giver : givers)
        giver.join();
    EXPECT_EQ(wrong, 0u);
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_EQ(queue.peek(), nullptr);
}

TEST(HandoffQueue, ATakerSleepsUntilTheNextHandOff)
{
    gm20b::HandoffQueue<Item> queue;
    std::thread taker([&] {
        for (uint64_t i = 0; i < 64; ++i) {
            EXPECT_EQ(queue.front().index, i);
            queue.pop();
        }
    });
    for (uint64_t i = 0; i < 64; ++i) {
        if (i % 8 == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        queue.push({i, {}});
    }
    taker.join();
    EXPECT_EQ(queue.size(), 0u);
}

// The size counts an item from its hand-off until the taker pops it, while the taker still works on it too
TEST(HandoffQueue, TheSizeCountsAnItemUntilItsPop)
{
    gm20b::HandoffQueue<Item> queue;
    queue.push({1, {}});
    queue.push({2, {}});
    EXPECT_EQ(queue.size(), 2u);
    EXPECT_EQ(queue.front().index, 1u);
    EXPECT_EQ(queue.size(), 2u);
    queue.pop();
    EXPECT_EQ(queue.size(), 1u);
    queue.pop();
    EXPECT_EQ(queue.size(), 0u);
}

// As the completion thread sleeps on quiet ranges: their hand-offs do not wake it, a wake() after a change it reads does
TEST(HandoffQueue, QuietHandOffsLeaveTheTakerAsleepUntilAWake)
{
    gm20b::HandoffQueue<Item> queue;
    std::atomic<bool> promised{true}, woken{false};
    size_t seen = 0;
    std::thread taker([&] {
        queue.waitFor([&](const gm20b::HandoffQueue<Item> &q) {
            seen = length(q);
            return !promised.load();
        });
        woken = true;
    });
    for (uint64_t i = 0; i < 3; ++i)
        queue.push({i, {}, true}, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(woken.load());
    promised = false;
    queue.wake();
    taker.join();
    EXPECT_EQ(seen, 3u);
}

// A take appends to what the taker holds, so the list it reads stays in hand-off order across takes
TEST(HandoffQueue, TheTakersListKeepsItsOrderAcrossTakes)
{
    gm20b::HandoffQueue<Item> queue;
    queue.push({1, {}});
    queue.push({2, {}});
    EXPECT_EQ(queue.front().index, 1u);
    queue.push({3, {}});
    queue.waitFor([](const gm20b::HandoffQueue<Item> &q) {
        return length(q) == 3;
    });
    EXPECT_EQ(queue.at(0).index, 1u);
    EXPECT_EQ(queue.at(2).index, 3u);
    queue.pop();
    EXPECT_EQ(queue.front().index, 2u);
    queue.pop();
    queue.pop();
    queue.push({4, {}});
    EXPECT_EQ(queue.front().index, 4u);
    queue.pop();
    EXPECT_EQ(queue.size(), 0u);
}

// The completion thread over quiet ranges (TegraGM20BMetalFinish.mm completionLoop): their hand-offs wake nothing while the submit
// thread promises a range that interrupts behind them; the promise's end wakes it onto the newest, a range that interrupts
// onto that one
TEST(HandoffQueue, TheCompletionThreadTakesItsTargetOverQuietRanges)
{
    gm20b::HandoffQueue<Item> queue;
    std::atomic<bool> promised{true};
    std::atomic<size_t> target{~size_t(0)};
    std::thread completion([&] {
        std::optional<size_t> found;
        queue.waitFor([&](const gm20b::HandoffQueue<Item> &q) {
            found = gm20b::completionTarget(q, promised.load(), false);
            return found.has_value();
        });
        target = *found;
    });
    queue.push({1, {}, true}, false);
    queue.push({2, {}, true}, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(target.load(), ~size_t(0)) << "asleep on quiet ranges under a promise";
    promised = false;
    queue.wake();
    completion.join();
    EXPECT_EQ(target.load(), 1u) << "the newest quiet range";

    gm20b::HandoffQueue<Item> loud;
    std::thread next([&] {
        std::optional<size_t> found;
        loud.waitFor([&](const gm20b::HandoffQueue<Item> &q) {
            found = gm20b::completionTarget(q, true, false);
            return found.has_value();
        });
        target = *found;
    });
    loud.push({1, {}, true}, false);
    loud.push({2, {}, false});
    next.join();
    EXPECT_EQ(target.load(), 1u) << "the first range that interrupts";
}

TEST(HandoffQueue, TimedSleepWakesAtDeadlineAndEarlyOnWake)
{
    gm20b::HandoffQueue<Item> queue;
    const auto t0 = std::chrono::steady_clock::now();
    queue.sleep(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_MSEC));
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_GE(elapsedMs, 10);

    std::atomic<bool> woke{false};
    std::thread sleeper([&] {
        queue.sleep(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        woke.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(woke.load(std::memory_order_acquire));
    const auto wakeAt = std::chrono::steady_clock::now();
    queue.wake();
    sleeper.join();
    const auto wakeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wakeAt).count();
    EXPECT_TRUE(woke.load(std::memory_order_acquire));
    EXPECT_LT(wakeMs, 500);
}

}  // namespace
