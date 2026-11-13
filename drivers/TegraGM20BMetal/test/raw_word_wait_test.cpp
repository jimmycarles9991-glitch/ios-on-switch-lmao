// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_word_wait.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

// As waitUntilCompleted against the completion: a change made at any point of a waiter's check wakes it, in every
// interleaving, and a waiter whose state is already done returns without sleeping
TEST(ChangeWord, NoWaiterSleepsThroughTheChangeItWaitsFor)
{
    constexpr int kRounds = 3000, kWaiters = 3;
    for (int round = 0; round < kRounds; round++) {
        raw::ChangeWord word;
        std::atomic<bool> done{false};
        std::atomic<int> returned{0};
        std::vector<std::thread> waiters;
        for (int w = 0; w < kWaiters; w++)
            waiters.emplace_back([&] {
                word.waitFor([&] {
                    return done.load();
                });
                returned++;
            });
        if (round % 2)
            std::this_thread::yield();
        done = true;
        word.changed();
        for (std::thread &t : waiters)
            t.join();
        ASSERT_EQ(returned.load(), kWaiters) << round;
    }
}

TEST(ChangeWord, AChangeThatLeavesTheStateUndoneKeepsTheWaiterWaiting)
{
    raw::ChangeWord word;
    std::atomic<int> step{0};
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        word.waitFor([&] {
            return step.load() == 2;
        });
        returned = true;
    });
    step = 1;
    word.changed();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(returned.load());
    step = 2;
    word.changed();
    waiter.join();
    EXPECT_TRUE(returned.load());
}

}  // namespace
