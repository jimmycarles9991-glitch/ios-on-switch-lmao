// SPDX-License-Identifier: MIT
// follow.h on the host: the driver's threads take the fixed priority of the threads that commit to
// them (Core Animation's SCHED_FIFO render threads), never a lower one, and keep their own when the committer is
// timeshare.
#include <gtest/gtest.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <utility>

#include "../follow.h"

namespace {

// A follower thread: joined, then parked until the test ends it.
class Follower {
public:
    explicit Follower(gm20b::Followers &f)
        : thread_([this, &f] {
              f.join();
              std::unique_lock<std::mutex> l(lock_);
              self_ = pthread_self();
              joined_ = true;
              changed_.notify_all();
              changed_.wait(l, [this] {
                  return done_;
              });
          })
    {
        std::unique_lock<std::mutex> l(lock_);
        changed_.wait(l, [this] {
            return joined_;
        });
    }

    ~Follower()
    {
        {
            std::lock_guard<std::mutex> l(lock_);
            done_ = true;
        }
        changed_.notify_all();
        thread_.join();
    }

    std::pair<int, int> policy()
    {
        int policy = 0;
        sched_param param{};
        EXPECT_EQ(pthread_getschedparam(self_, &policy, &param), 0);
        return {policy, param.sched_priority};
    }

private:
    std::mutex lock_;
    std::condition_variable changed_;
    bool joined_ = false, done_ = false;
    pthread_t self_{};
    std::thread thread_;
};

// One commit from a new thread (a fixed-priority committer is read once) at this policy, or at its default with
// SCHED_OTHER.
void CommitFrom(gm20b::Followers &f, int policy, int priority)
{
    std::thread([&f, policy, priority] {
        if (policy != SCHED_OTHER) {
            sched_param param{};
            param.sched_priority = priority;
            ASSERT_EQ(pthread_setschedparam(pthread_self(), policy, &param), 0);
        }
        f.committing();
    }).join();
}

int DefaultPolicy()
{
    int policy = 0;
    sched_param param{};
    pthread_getschedparam(pthread_self(), &policy, &param);
    return policy;
}

}  // namespace

TEST(GM20BFollow, TimeshareCommittersLeaveFollowersAlone)
{
    gm20b::Followers f;
    Follower a(f);
    const std::pair<int, int> before = a.policy();
    CommitFrom(f, SCHED_OTHER, 0);
    EXPECT_EQ(a.policy(), before);
    EXPECT_EQ(before.first, DefaultPolicy());
}

TEST(GM20BFollow, FollowersTakeAFixedCommittersPolicyNeverALowerOne)
{
    gm20b::Followers f;
    Follower a(f), b(f);
    CommitFrom(f, SCHED_FIFO, 40);
    EXPECT_EQ(a.policy(), std::make_pair(SCHED_FIFO, 40));
    EXPECT_EQ(b.policy(), std::make_pair(SCHED_FIFO, 40));
    CommitFrom(f, SCHED_FIFO, 39);
    EXPECT_EQ(a.policy(), std::make_pair(SCHED_FIFO, 40));
    CommitFrom(f, SCHED_FIFO, 41);
    EXPECT_EQ(b.policy(), std::make_pair(SCHED_FIFO, 41));
}

TEST(GM20BFollow, ALaterFollowerStartsAtTheClassSoFar)
{
    gm20b::Followers f;
    CommitFrom(f, SCHED_FIFO, 40);
    {
        Follower gone(f);  // ends before the next raise: it has left the list
    }
    Follower late(f);
    EXPECT_EQ(late.policy(), std::make_pair(SCHED_FIFO, 40));
    CommitFrom(f, SCHED_FIFO, 42);
    EXPECT_EQ(late.policy(), std::make_pair(SCHED_FIFO, 42));
}
