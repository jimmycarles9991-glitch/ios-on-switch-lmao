// SPDX-License-Identifier: MIT
#include "../ordered_executor.h"
#import <Foundation/Foundation.h>
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

class Latch {
public:
    void open()
    {
        std::lock_guard<std::mutex> l(lock_);
        open_ = true;
        changed_.notify_all();
    }

    void wait()
    {
        std::unique_lock<std::mutex> l(lock_);
        changed_.wait(l, [&] {
            return open_;
        });
    }

private:
    std::mutex lock_;
    std::condition_variable changed_;
    bool open_ = false;
};

std::unique_ptr<gm20b::OrderedExecutor> started()
{
    auto exec = std::make_unique<gm20b::OrderedExecutor>();
    int err = -1;
    EXPECT_TRUE(exec->start(err));
    EXPECT_EQ(err, 0);
    return exec;
}

// As the submit worker, the completion thread and a late addScheduledHandler give it handlers: each giver's run in the
// order it gave them
TEST(OrderedExecutor, EachGiversItemsRunInTheOrderItGaveThem)
{
    constexpr int kGivers = 4, kItems = 2000;
    auto exec = started();
    std::vector<std::vector<int>> ran(kGivers);
    std::atomic<int> left{kGivers * kItems};
    Latch done;
    std::vector<std::thread> givers;
    for (int g = 0; g < kGivers; g++)
        givers.emplace_back([&, g] {
            for (int i = 0; i < kItems; i++)
                exec->enqueue([&, g, i] {
                    ran[size_t(g)].push_back(i);
                    if (left.fetch_sub(1) == 1)
                        done.open();
                });
        });
    for (std::thread &t : givers)
        t.join();
    done.wait();
    for (int g = 0; g < kGivers; g++) {
        ASSERT_EQ(ran[size_t(g)].size(), size_t(kItems)) << g;
        for (int i = 0; i < kItems; i++)
            ASSERT_EQ(ran[size_t(g)][size_t(i)], i) << "giver " << g;
    }
}

// waitUntilScheduled donates to ownerPort() from before the first item: it is the port of the thread the items run on
TEST(OrderedExecutor, OwnerPortIsTheItemsThreadFromItsStart)
{
    auto exec = started();
    const uint32_t port = exec->ownerPort();
    EXPECT_NE(port, 0u);
    std::atomic<uint32_t> ranOn{0};
    Latch done;
    exec->enqueue([&] {
        ranOn = uint32_t(pthread_mach_thread_np(pthread_self()));
        done.open();
    });
    done.wait();
    EXPECT_EQ(ranOn.load(), port);
}

// Once per Core Animation frame: a hand-off takes no lock
TEST(OrderedExecutor, EnqueueTakesNoLock)
{
    auto exec = started();
    Latch done;
    uint64_t before = 0, after = 0, waits = 0, ticks = 0;
    gm20b::LockCount::here(before, waits, ticks);
    for (int i = 0; i < 100; i++)
        exec->enqueue([] {});
    gm20b::LockCount::here(after, waits, ticks);
    exec->enqueue([&] {
        done.open();
    });
    done.wait();
    EXPECT_EQ(after - before, 0u);
}

// A queue's last release can come from a lambda's captures on its own handler thread: the executor is destroyed there,
// and what was queued before still runs
TEST(OrderedExecutor, DestroyedFromInsideItsOwnHandlerRunsWhatWasQueued)
{
    auto exec = started();
    gm20b::OrderedExecutor *raw = exec.get();
    Latch go, done;
    std::atomic<bool> laterRan{false};
    raw->enqueue([&go, owned = std::shared_ptr<gm20b::OrderedExecutor>(std::move(exec))]() mutable {
        go.wait();
        owned.reset();
    });
    raw->enqueue([&] {
        laterRan = true;
        done.open();
    });
    go.open();
    done.wait();
    EXPECT_TRUE(laterRan.load());
}

// Destroyed by another thread with items still queued: it returns at once, they run in order, and every item's captures
// are released
TEST(OrderedExecutor, DestroyedWithItemsQueuedRunsAndReleasesThem)
{
    constexpr int kItems = 64;
    auto exec = started();
    Latch go, done, released;
    std::shared_ptr<int> token(new int(0), [&released](int *p) {
        delete p;
        released.open();
    });
    std::vector<int> ran;
    exec->enqueue([&go] {
        go.wait();
    });
    for (int i = 0; i < kItems; i++)
        exec->enqueue([&, i, token] {
            ran.push_back(i);
            if (i == kItems - 1)
                done.open();
        });
    token.reset();
    exec.reset();
    go.open();
    done.wait();
    released.wait();
    ASSERT_EQ(ran.size(), size_t(kItems));
    for (int i = 0; i < kItems; i++)
        EXPECT_EQ(ran[size_t(i)], i);
}

struct Watched {
    __weak id inRun;
    __weak id inRelease;
};

void autoreleaseWatched(__weak id &watch)
{
    __autoreleasing id object = [NSObject new];
    watch = object;
}

// An item's own autoreleases (a handler's) and its captures' (a command buffer's dealloc) are drained before the next
// item: the thread lives as long as its queue, and no pool of the thread's own drains before it ends
TEST(OrderedExecutor, EachItemDrainsItsAutoreleasesAndItsCapturesBeforeTheNext)
{
    struct ReleasesOnLast {
        Watched *watched = nullptr;

        ~ReleasesOnLast()
        {
            autoreleaseWatched(watched->inRelease);
        }
    };

    auto exec = started();
    Watched watched;
    Latch go, done;
    std::atomic<bool> runGone{false}, releaseGone{false};
    auto captured = std::make_shared<ReleasesOnLast>();
    captured->watched = &watched;
    // held until this thread has let its reference go, so the capture's last release is the executor's
    exec->enqueue([&go] {
        go.wait();
    });
    exec->enqueue([&watched, captured] {
        autoreleaseWatched(watched.inRun);
    });
    captured.reset();
    go.open();
    exec->enqueue([&] {
        runGone = watched.inRun == nil;
        releaseGone = watched.inRelease == nil;
        done.open();
    });
    done.wait();
    EXPECT_TRUE(runGone.load());
    EXPECT_TRUE(releaseGone.load());
}

}  // namespace
