// SPDX-License-Identifier: MIT
// The raw path's own threads (submit, completion, the pool's grower) do work that a committing thread waits for, so they
// run at the class of the threads that commit to them. Core Animation's render server commits from fixed-priority threads
// (QuartzCore's CA::Render::Server::set_thread_priority: SCHED_FIFO 55 for the render server, 54 for the display), and
// XNU clamps a daemon's user-interactive QoS to user-initiated (37), so a QoS class alone put them below the thread
// waiting on them, level with every busy daemon. Committers at a QoS class (an app's threads) leave them at their own.
#pragma once
#include "raw_lock.h"
#include <algorithm>
#include <mutex>
#include <pthread.h>
#include <vector>

namespace raw {

class Followers {
public:
    // On the follower itself, as it starts: it takes the class committers have shown so far, and leaves the list as its
    // thread ends.
    void join()
    {
        struct Member {
            Followers *of;

            ~Member()
            {
                of->leave(pthread_self());
            }
        };

        static thread_local Member member{this};
        std::lock_guard<raw::Mutex> l(lock_);
        threads_.push_back(pthread_self());
        if (policy_ != SCHED_OTHER)
            pthread_setschedparam(pthread_self(), policy_, &param_);
    }

    // On a committing thread. libpthread keeps a thread's policy in its pthread (pthread_getschedparam makes no call).
    // A fixed-priority committer is read once; a timeshare one at each commit, since Core Animation sets its threads'
    // priority from the thread that created them, which can come after the new thread's first commit.
    void committing()
    {
        static thread_local bool fixed;
        if (fixed)
            return;
        int policy;
        sched_param param;
        if (pthread_getschedparam(pthread_self(), &policy, &param) != 0 || policy == SCHED_OTHER)
            return;
        fixed = true;
        std::lock_guard<raw::Mutex> l(lock_);
        if (policy_ != SCHED_OTHER && param.sched_priority <= param_.sched_priority)
            return;
        policy_ = policy;
        param_ = param;
        for (pthread_t t : threads_)
            pthread_setschedparam(t, policy_, &param_);
    }

private:
    void leave(pthread_t self)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        threads_.erase(std::remove(threads_.begin(), threads_.end(), self), threads_.end());
    }

    raw::Mutex lock_;
    std::vector<pthread_t> threads_;
    int policy_ = SCHED_OTHER;
    sched_param param_{};
};

// Never destroyed: the submit and completion threads are detached and may join or leave while the process exits, after
// its static destructors ran (a destroyed mutex throws, and the process aborts)
inline Followers &followers()
{
    static Followers &f = *new Followers;
    return f;
}

}  // namespace raw
