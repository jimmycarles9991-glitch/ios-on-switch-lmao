// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace raw {

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The locks the runtime took through Mutex, RecursiveMutex and CondVar's wakes, on every thread since the process began
// (gpu.path.encode_cost's ceiling). Each thread counts its own with a plain store, no read-modify-write or barrier; a
// read sums the threads that took one, and those that ended.
class LockCount {
public:
    static void taken()
    {
        Thread &t = mine();
        t.taken.store(t.taken.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    static uint64_t total()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> l(r.lock);
        uint64_t sum = r.ended;
        for (const Thread *t = r.threads; t; t = t->next)
            sum += t->taken.load(std::memory_order_relaxed);
        return sum;
    }

private:
    struct Thread;

    struct Registry {
        std::mutex lock;
        Thread *threads = nullptr;
        uint64_t ended = 0;
    };

    struct Thread {
        std::atomic<uint64_t> taken{0};
        Thread *next = nullptr, *prev = nullptr;

        Thread()
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> l(r.lock);
            next = r.threads;
            if (next)
                next->prev = this;
            r.threads = this;
        }

        ~Thread()
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> l(r.lock);
            r.ended += taken.load(std::memory_order_relaxed);
            (prev ? prev->next : r.threads) = next;
            if (next)
                next->prev = prev;
        }
    };

    // never destroyed: a thread may end after the process's statics did
    static Registry &registry()
    {
        static Registry *r = new Registry;
        return *r;
    }

    static Thread &mine()
    {
        static thread_local Thread t;
        return t;
    }
};

inline void countLock()
{
    LockCount::taken();
}
#else
inline void countLock()
{}
#endif

// A capability to -Wthread-safety as the std mutex it wraps is; its own body is not analysed (it holds the std one)
template <typename Native> class __attribute__((capability("mutex"))) CountedMutex {
public:
    void lock() __attribute__((acquire_capability(), no_thread_safety_analysis))
    {
        m_.lock();
        countLock();
    }

    bool try_lock() __attribute__((try_acquire_capability(true), no_thread_safety_analysis))
    {
        if (!m_.try_lock())
            return false;
        countLock();
        return true;
    }

    void unlock() __attribute__((release_capability(), no_thread_safety_analysis))
    {
        m_.unlock();
    }

    Native &native()
    {
        return m_;
    }

    // The files the raw stack key hashes (//gpu:raw_stack: raw_completed.cpp's Device::addRecord) lock it as the std
    // mutex they name, uncounted: a lock taken per runtime record made, never per command buffer
    operator Native &()
    {
        return m_;
    }

private:
    Native m_;
};

using Mutex = CountedMutex<std::mutex>;
using RecursiveMutex = CountedMutex<std::recursive_mutex>;

// std::condition_variable over a Mutex: a wake takes its lock again, and counts it
class CondVar {
public:
    void notify_one() noexcept
    {
        cv_.notify_one();
    }

    void notify_all() noexcept
    {
        cv_.notify_all();
    }

    void wait(std::unique_lock<Mutex> &l) __attribute__((no_thread_safety_analysis))
    {
        std::unique_lock<std::mutex> native(l.mutex()->native(), std::adopt_lock);
        cv_.wait(native);
        native.release();
        countLock();
    }

    template <typename Ready> void wait(std::unique_lock<Mutex> &l, Ready ready)
    {
        while (!ready())
            wait(l);
    }

    template <typename Clock, typename Duration>
    std::cv_status wait_until(std::unique_lock<Mutex> &l, const std::chrono::time_point<Clock, Duration> &until)
        __attribute__((no_thread_safety_analysis))
    {
        std::unique_lock<std::mutex> native(l.mutex()->native(), std::adopt_lock);
        const std::cv_status status = cv_.wait_until(native, until);
        native.release();
        countLock();
        return status;
    }

    template <typename Clock, typename Duration, typename Ready>
    bool wait_until(std::unique_lock<Mutex> &l, const std::chrono::time_point<Clock, Duration> &until, Ready ready)
    {
        while (!ready())
            if (wait_until(l, until) == std::cv_status::timeout)
                return ready();
        return true;
    }

    template <typename Rep, typename Period, typename Ready>
    bool wait_for(std::unique_lock<Mutex> &l, const std::chrono::duration<Rep, Period> &wait, Ready ready)
    {
        return wait_until(l, std::chrono::steady_clock::now() + wait, std::move(ready));
    }

private:
    std::condition_variable cv_;
};

}  // namespace raw
