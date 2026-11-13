// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "../raw/runtime/raw_handler_order.h"

namespace {

// RawMetalFinish.mm's finish(): a command buffer's completed handlers run once its predecessor's have, handed to the
// predecessor when they have not
struct Buffer {
    int id = 0;
    Buffer *before = nullptr;
    raw::HandlerTurn turn;
};

class Queue {
public:
    explicit Queue(size_t n) : buffers_(n)
    {
        for (size_t i = 0; i < n; ++i) {
            buffers_[i].id = int(i);
            buffers_[i].before = i ? &buffers_[i - 1] : nullptr;
        }
    }

    void finish(size_t i)
    {
        Buffer &b = buffers_[i];
        if (b.before && !b.before->turn.ran()) {
            auto *later = new std::function<void()>([this, &b] {
                run(b);
            });
            if (b.before->turn.hand(later))
                return;
            delete later;
        }
        run(b);
    }

    std::vector<int> ran()
    {
        std::lock_guard<std::mutex> l(lock_);
        return ran_;
    }

private:
    void run(Buffer &b)
    {
        {
            std::lock_guard<std::mutex> l(lock_);
            ran_.push_back(b.id);
        }
        if (void *next = b.turn.finished()) {
            auto *later = static_cast<std::function<void()> *>(next);
            (*later)();
            delete later;
        }
    }

    std::vector<Buffer> buffers_;
    std::mutex lock_;
    std::vector<int> ran_;
};

TEST(HandlerTurn, WorkHandedBeforeThePredecessorRanWaitsForIt)
{
    Queue q(2);
    q.finish(1);
    EXPECT_TRUE(q.ran().empty());
    q.finish(0);
    EXPECT_EQ(q.ran(), (std::vector<int>{0, 1}));
}

TEST(HandlerTurn, WorkAfterThePredecessorRanRunsAtOnce)
{
    raw::HandlerTurn turn;
    EXPECT_FALSE(turn.ran());
    EXPECT_EQ(turn.finished(), nullptr);
    EXPECT_TRUE(turn.ran());
    int work = 0;
    EXPECT_FALSE(turn.hand(&work));
}

TEST(HandlerTurn, CommandBuffersFinishedOnRacingThreadsRunInEnqueueOrder)
{
    constexpr size_t kBuffers = 4;
    for (int trial = 0; trial < 500; ++trial) {
        Queue q(kBuffers);
        std::atomic<size_t> ready{0};
        std::vector<std::thread> threads;
        for (size_t i = 0; i < kBuffers; ++i)
            threads.emplace_back([&, i] {
                ready++;
                while (ready.load() < kBuffers) {
                }
                q.finish(kBuffers - 1 - i);
            });
        for (std::thread &t : threads)
            t.join();
        ASSERT_EQ(q.ran(), (std::vector<int>{0, 1, 2, 3})) << "trial " << trial;
    }
}

}  // namespace
