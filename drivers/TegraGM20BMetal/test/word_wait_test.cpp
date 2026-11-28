// SPDX-License-Identifier: MIT
#include "../word_wait.h"
#include <gtest/gtest.h>
#include <mach/mach.h>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

// Until every thread of `ports` is blocked (TH_STATE_WAITING): a waiter inside its __ulock_wait. A fixed sleep was not
// enough on a busy test host: a waiter still runnable when the test went on saw `done` already set, rightly did not sleep,
// and returned false. False: one never blocked within 5 s.
bool waitUntilBlocked(const std::vector<std::atomic<uint32_t>> &ports)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        bool all = true;
        for (const std::atomic<uint32_t> &port : ports) {
            thread_basic_info_data_t info = {};
            mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
            const uint32_t p = port.load();
            all = all && p &&
                  thread_info(p, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS &&
                  info.run_state == TH_STATE_WAITING;
        }
        if (all)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

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
    // a result a waiter thread each: std::vector<bool> packs them into one word, so the threads' writes raced and one
    // was lost (a waiter woken with __ulock_wait's 1 read back false)
    std::vector<std::atomic<bool>> firstResults(kWaiters), secondResults(kWaiters);
    std::vector<std::atomic<uint32_t>> ports(kWaiters);
    gm20b::ChangeWord stepGate;

    std::vector<std::thread> waiters;
    for (int w = 0; w < kWaiters; ++w)
        waiters.emplace_back([&, w] {
            ports[size_t(w)] = gm20b::OwnedWord::currentOwner();
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
    ASSERT_TRUE(waitUntilBlocked(ports));
    EXPECT_EQ(firstWoke.load(), 0);

    // Replacing owner while done == false wakes all waiters; N-1 observe __ulock_wait > 0.
    word.setOwner(owner2Port.load());
    stepGate.waitFor([&] {
        return firstWoke.load() == kWaiters;
    });
    ASSERT_TRUE(waitUntilBlocked(ports));
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
    for (std::atomic<uint32_t> &port : ports)
        port = 0;
    for (int w = 0; w < kWaiters; ++w)
        waiters.emplace_back([&, w] {
            ports[size_t(w)] = gm20b::OwnedWord::currentOwner();
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
    ASSERT_TRUE(waitUntilBlocked(ports));
    word.setOwner(owner2Port.load());
    ASSERT_TRUE(waitUntilBlocked(ports));
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

// As the submit worker hands a command buffer to its queue's handler thread: the worker's own clear after the hand-off
// leaves the handler thread owning it, so a waiter keeps donating to that thread until it clears its own ownership
TEST(OwnedWord, ClearIfLeavesAHandedOnOwnerInPlace)
{
    gm20b::OwnedWord word;
    std::atomic<uint32_t> workerPort{0}, handlerPort{0};
    std::atomic<bool> releaseOwners{false};
    gm20b::ChangeWord ownerGate;
    const auto owner = [&](std::atomic<uint32_t> &port) {
        return std::thread([&] {
            port.store(gm20b::OwnedWord::currentOwner());
            ownerGate.changed();
            ownerGate.waitFor([&] {
                return releaseOwners.load();
            });
        });
    };
    std::thread worker = owner(workerPort), handler = owner(handlerPort);
    ownerGate.waitFor([&] {
        return workerPort.load() != 0 && handlerPort.load() != 0;
    });

    word.setOwner(workerPort.load());
    word.setOwner(handlerPort.load());
    word.clearIf(workerPort.load());
    EXPECT_EQ(word.owner(), handlerPort.load());
    word.clearIf(0);
    EXPECT_EQ(word.owner(), handlerPort.load());

    std::atomic<bool> done{false}, returned{false};
    std::thread waiter([&] {
        word.waitWhileOwned([&] {
            return done.load();
        });
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    word.clearIf(workerPort.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_FALSE(returned.load());
    EXPECT_EQ(word.owner(), handlerPort.load());

    done = true;
    word.clearIf(handlerPort.load());
    waiter.join();
    EXPECT_TRUE(returned.load());
    EXPECT_FALSE(word.hasOwner());

    releaseOwners = true;
    ownerGate.changed();
    worker.join();
    handler.join();
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
