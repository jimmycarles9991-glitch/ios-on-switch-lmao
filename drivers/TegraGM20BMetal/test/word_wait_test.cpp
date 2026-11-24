// SPDX-License-Identifier: MIT
#include "../word_wait.h"
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
        gm20b::ChangeWord word;
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
    gm20b::ChangeWord word;
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

TEST(ChangeWord, WaitOnceReturnsWhenWordChanges)
{
    gm20b::ChangeWord word;
    std::atomic<bool> ready{false};
    std::atomic<bool> woke{false};
    std::thread waiter([&] {
        word.waitOnce([&] {
            return ready.load();
        });
        woke = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(woke.load());
    ready = true;
    word.changed();
    waiter.join();
    EXPECT_TRUE(woke.load());
}

TEST(OwnedWord, NoWaiterSleepsThroughOwnerClearFromOwnerOrNonOwner)
{
    constexpr int kRounds = 1000, kWaiters = 3;
    for (int round = 0; round < kRounds; round++) {
        gm20b::OwnedWord word;
        std::atomic<bool> done{false};
        std::atomic<int> returned{0};
        std::atomic<uint32_t> ownerPort{0};
        std::atomic<bool> releaseOwner{false};
        gm20b::ChangeWord ownerGate;

        std::thread owner([&] {
            ownerPort.store(gm20b::OwnedWord::currentOwner());
            word.setOwner(ownerPort.load());
            ownerGate.changed();
            ownerGate.waitFor([&] {
                return releaseOwner.load();
            });
            if (round % 2 == 0) {
                done = true;
                word.clear();
            }
        });
        ownerGate.waitFor([&] {
            return ownerPort.load() != 0;
        });

        std::vector<std::thread> waiters;
        for (int w = 0; w < kWaiters; w++)
            waiters.emplace_back([&] {
                word.waitWhileOwned([&] {
                    return done.load();
                });
                returned++;
            });
        if (round % 3)
            std::this_thread::yield();
        if (round % 2 == 1) {
            done = true;
            word.clear();  // non-owner wake (ULF_WAKE_ALLOW_NON_OWNER)
        }
        releaseOwner = true;
        ownerGate.changed();
        for (std::thread &t : waiters)
            t.join();
        owner.join();
        ASSERT_EQ(returned.load(), kWaiters) << round;
    }
}

TEST(OwnedWord, MultiWaiterOwnerHandoffReturnsTrueForEveryWaiter)
{
    constexpr int kWaiters = 4;
    gm20b::OwnedWord word;
    std::atomic<bool> done{false};
    std::atomic<uint32_t> owner1Port{0}, owner2Port{0};
    std::atomic<bool> releaseOwners{false};
    gm20b::ChangeWord ownerGate;

    std::thread owner1([&] {
        owner1Port.store(gm20b::OwnedWord::currentOwner());
        ownerGate.changed();
        ownerGate.waitFor([&] {
            return releaseOwners.load();
        });
    });
    std::thread owner2([&] {
        owner2Port.store(gm20b::OwnedWord::currentOwner());
        ownerGate.changed();
        ownerGate.waitFor([&] {
            return releaseOwners.load();
        });
    });
    ownerGate.waitFor([&] {
        return owner1Port.load() != 0 && owner2Port.load() != 0;
    });

    word.setOwner(owner1Port.load());
    std::atomic<int> entered{0}, firstWoke{0}, finished{0};
    std::vector<bool> firstResults(kWaiters, false), secondResults(kWaiters, false);
    gm20b::ChangeWord stepGate;

    std::vector<std::thread> waiters;
    for (int w = 0; w < kWaiters; ++w)
        waiters.emplace_back([&, w] {
            entered++;
            stepGate.changed();
            firstResults[size_t(w)] = word.waitOnce([&] {
                return done.load();
            });
            firstWoke++;
            stepGate.changed();
            secondResults[size_t(w)] = word.waitOnce([&] {
                return done.load();
            });
            finished++;
        });

    stepGate.waitFor([&] {
        return entered.load() == kWaiters;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_EQ(firstWoke.load(), 0);

    // Replacing owner while done == false wakes all waiters; N-1 observe __ulock_wait > 0.
    word.setOwner(owner2Port.load());
    stepGate.waitFor([&] {
        return firstWoke.load() == kWaiters;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_EQ(finished.load(), 0);

    done = true;
    word.clear();
    for (std::thread &t : waiters)
        t.join();
    EXPECT_EQ(finished.load(), kWaiters);
    for (int w = 0; w < kWaiters; ++w) {
        EXPECT_TRUE(firstResults[size_t(w)]) << "waiter " << w << " rejected positive __ulock_wait return on handoff";
        EXPECT_TRUE(secondResults[size_t(w)]) << "waiter " << w << " rejected positive __ulock_wait return on clear";
    }

    // waitWhileOwned must stay asleep across owner1 -> owner2 handoff until done becomes true.
    done = false;
    word.setOwner(owner1Port.load());
    entered = 0;
    finished = 0;
    waiters.clear();
    for (int w = 0; w < kWaiters; ++w)
        waiters.emplace_back([&] {
            entered++;
            stepGate.changed();
            word.waitWhileOwned([&] {
                return done.load();
            });
            finished++;
        });
    stepGate.waitFor([&] {
        return entered.load() == kWaiters;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    word.setOwner(owner2Port.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_EQ(finished.load(), 0);
    done = true;
    word.clear();
    for (std::thread &t : waiters)
        t.join();
    EXPECT_EQ(finished.load(), kWaiters);

    releaseOwners = true;
    ownerGate.changed();
    owner1.join();
    owner2.join();
}

TEST(OwnedWord, SelfOwnedWaitDoesNotBlockOrSpin)
{
    gm20b::OwnedWord word;
    word.setOwner(gm20b::OwnedWord::currentOwner());
    EXPECT_TRUE(word.hasOwner());
    EXPECT_FALSE(word.waitOnce([] {
        return false;
    }));
    word.clear();
    EXPECT_FALSE(word.hasOwner());
}

}  // namespace
