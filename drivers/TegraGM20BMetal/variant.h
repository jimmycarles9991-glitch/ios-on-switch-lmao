// SPDX-License-Identifier: MIT
#pragma once
#include "lock.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace gm20b {

template <typename T> class Variant : public std::enable_shared_from_this<Variant<T>> {
public:
    enum class State : uint8_t {
        Pending,
        Ready,
        Refused,  // the driver has no form of it
        Failed,   // no compiler made it: what needs it fails, as a pipeline Metal could not compile
    };

    State state() const
    {
        return state_.load(std::memory_order_acquire);
    }

    T *value() const
    {
        return state() == State::Ready ? value_ : nullptr;
    }

    using WaitFn = std::function<bool()>;

    State observe(std::function<void()> observer)
    {
        const State ready = state();
        if (ready != State::Pending)
            return ready;
        std::lock_guard<gm20b::Mutex> lock(lock_);
        const State current = state();
        if (current == State::Pending && observer)
            observers_.push_back(std::move(observer));
        return current;
    }

    void setWait(WaitFn wait)
    {
        std::vector<std::function<void()>> notify;
        {
            std::lock_guard<gm20b::Mutex> lock(lock_);
            if (state() != State::Pending)
                return;
            wait_ = std::move(wait);
            notify = waitObservers_;
        }
        for (auto &observer : notify)
            observer();
    }

    void observeWait(std::function<void()> observer)
    {
        if (!observer)
            return;
        bool already = false;
        {
            std::lock_guard<gm20b::Mutex> lock(lock_);
            if (state() != State::Pending)
                return;
            already = static_cast<bool>(wait_);
            waitObservers_.push_back(observer);
        }
        if (already)
            observer();
    }

    bool waitPending() const
    {
        WaitFn fn;
        {
            std::lock_guard<gm20b::Mutex> lock(lock_);
            if (state() != State::Pending || !wait_)
                return false;
            fn = wait_;
        }
        return fn();
    }

    bool publish(T *value)
    {
        return settle(value, value ? State::Ready : State::Refused);
    }

    bool fail()
    {
        return settle(nullptr, State::Failed);
    }

    // Failed with no compiler to ask (none serves yet, or none this boot): no answer about the variant, so reopen()
    // lets the next ask make it
    bool failUnserved()
    {
        return settle(nullptr, State::Failed, true);
    }

    // Pending again after failUnserved(): true for the one caller that is to ask for it
    bool reopen()
    {
        if (!unserved_.load(std::memory_order_acquire))
            return false;
        std::lock_guard<gm20b::Mutex> lock(lock_);
        if (!unserved_.load(std::memory_order_relaxed))
            return false;
        unserved_.store(false, std::memory_order_relaxed);
        state_.store(State::Pending, std::memory_order_release);
        return true;
    }

private:
    bool settle(T *value, State settled, bool unserved = false)
    {
        std::vector<std::function<void()>> observers, waitObservers;
        WaitFn wait;
        {
            std::lock_guard<gm20b::Mutex> lock(lock_);
            if (state() != State::Pending)
                return false;
            value_ = value;
            unserved_.store(unserved, std::memory_order_release);
            state_.store(settled, std::memory_order_release);
            observers.swap(observers_);
            waitObservers.swap(waitObservers_);
            wait = std::move(wait_);
        }
        for (auto &observer : observers)
            observer();
        return true;
    }

    mutable gm20b::Mutex lock_;
    std::atomic<State> state_{State::Pending};
    std::atomic<bool> unserved_{false};
    T *value_ = nullptr;
    std::vector<std::function<void()>> observers_, waitObservers_;
    WaitFn wait_;
};

}  // namespace gm20b
