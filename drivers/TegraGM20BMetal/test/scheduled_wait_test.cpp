// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "../scheduled_wait.h"

namespace {

using namespace std::chrono_literals;

// TegraGM20BMetalCommandBuffer.mm's waitUntilScheduled over TegraGM20BMetalSchedule.mm's parks and TegraGM20BMetalFinish.mm's failure wake.
// A park wakes the waiter (the hold marks it changed), which looks again.
class Buffer {
public:
    void park(gm20b::HeadWait)
    {
        changed_.notify_all();
    }

    void schedule()
    {
        {
            std::lock_guard<std::mutex> l(lock_);
            scheduled_ = true;
        }
        changed_.notify_all();
    }

    void fail()
    {
        {
            std::lock_guard<std::mutex> l(lock_);
            failed_ = true;
        }
        changed_.notify_all();
    }

    void waitUntilScheduled()
    {
        std::unique_lock<std::mutex> l(lock_);
        changed_.wait(l, [&] {
            return gm20b::scheduledWaitReturns(scheduled_, failed_);
        });
    }

private:
    std::mutex lock_;
    std::condition_variable changed_;
    bool scheduled_ = false, failed_ = false;
};

class Waiter {
public:
    explicit Waiter(Buffer &b)
        : thread_([this, &b] {
              b.waitUntilScheduled();
              returned_ = true;
          })
    {}

    ~Waiter()
    {
        thread_.join();
    }

    bool returnedWithin(std::chrono::milliseconds limit) const
    {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (!returned_ && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        return returned_;
    }

private:
    std::atomic<bool> returned_{false};
    std::thread thread_;
};

TEST(ScheduledWait, OnlyASharedEventWaitHoldsTheQueue)
{
    EXPECT_TRUE(gm20b::holds(gm20b::HeadWait::event));
    EXPECT_FALSE(gm20b::holds(gm20b::HeadWait::promised));
    EXPECT_FALSE(gm20b::holds(gm20b::HeadWait::compile));
    EXPECT_FALSE(gm20b::holds(gm20b::HeadWait::signal));
    EXPECT_FALSE(gm20b::holds(gm20b::HeadWait::order));
}

TEST(ScheduledWait, AHeadParkedOnACompileKeepsTheWaiterUntilItsWorkIsSubmitted)
{
    Buffer b;
    Waiter w(b);
    b.park(gm20b::HeadWait::compile);
    EXPECT_FALSE(w.returnedWithin(50ms));
    b.schedule();
    EXPECT_TRUE(w.returnedWithin(5000ms));
}

TEST(ScheduledWait, AFailedCompileWakesTheWaiter)
{
    Buffer b;
    Waiter w(b);
    b.park(gm20b::HeadWait::compile);
    EXPECT_FALSE(w.returnedWithin(50ms));
    b.fail();
    EXPECT_TRUE(w.returnedWithin(5000ms));
}

// Metal's waitUntilScheduled of a command buffer held on a shared event returns at its failure 5 s on (an M4 Max), or
// once the signal lets it be scheduled: never at the hold
TEST(ScheduledWait, AHeadParkedOnASharedEventKeepsTheWaiterUntilItFailsOrIsScheduled)
{
    {
        Buffer b;
        Waiter w(b);
        b.park(gm20b::HeadWait::event);
        EXPECT_FALSE(w.returnedWithin(50ms));
        b.fail();
        EXPECT_TRUE(w.returnedWithin(5000ms));
    }
    Buffer b;
    Waiter w(b);
    b.park(gm20b::HeadWait::event);
    EXPECT_FALSE(w.returnedWithin(50ms));
    b.schedule();
    EXPECT_TRUE(w.returnedWithin(5000ms));
}

}  // namespace
