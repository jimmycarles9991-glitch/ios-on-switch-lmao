// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "../raw/runtime/raw_scheduled_wait.h"

namespace {

using namespace std::chrono_literals;

// RawMetalCommandBuffer.mm's waitUntilScheduled over RawMetalSchedule.mm's parks and RawMetalFinish.mm's failure wake
class Buffer {
public:
    void park(raw::HeadWait wait)
    {
        if (!raw::holds(wait))
            return;
        {
            std::lock_guard<std::mutex> l(lock_);
            held_ = true;
        }
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
            return raw::scheduledWaitReturns(scheduled_, held_, failed_);
        });
    }

private:
    std::mutex lock_;
    std::condition_variable changed_;
    bool scheduled_ = false, held_ = false, failed_ = false;
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
    EXPECT_TRUE(raw::holds(raw::HeadWait::event));
    EXPECT_FALSE(raw::holds(raw::HeadWait::promised));
    EXPECT_FALSE(raw::holds(raw::HeadWait::compile));
    EXPECT_FALSE(raw::holds(raw::HeadWait::signal));
    EXPECT_FALSE(raw::holds(raw::HeadWait::order));
}

TEST(ScheduledWait, AHeadParkedOnACompileKeepsTheWaiterUntilItsWorkIsSubmitted)
{
    Buffer b;
    Waiter w(b);
    b.park(raw::HeadWait::compile);
    EXPECT_FALSE(w.returnedWithin(50ms));
    b.schedule();
    EXPECT_TRUE(w.returnedWithin(5000ms));
}

TEST(ScheduledWait, AFailedCompileWakesTheWaiter)
{
    Buffer b;
    Waiter w(b);
    b.park(raw::HeadWait::compile);
    EXPECT_FALSE(w.returnedWithin(50ms));
    b.fail();
    EXPECT_TRUE(w.returnedWithin(5000ms));
}

TEST(ScheduledWait, AHeadParkedOnASharedEventLetsTheWaiterGo)
{
    Buffer b;
    Waiter w(b);
    b.park(raw::HeadWait::signal);
    b.park(raw::HeadWait::order);
    EXPECT_FALSE(w.returnedWithin(50ms));
    b.park(raw::HeadWait::event);
    EXPECT_TRUE(w.returnedWithin(5000ms));
}

}  // namespace
