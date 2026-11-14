// SPDX-License-Identifier: MIT
#pragma once
#include "raw_lock.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace raw {

template <typename T> class Variant {
public:
    enum class State : uint8_t {
        Pending,
        Ready,
        Refused,  // the raw path has no form of it
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

    State observe(std::function<void()> observer)
    {
        const State ready = state();
        if (ready != State::Pending)
            return ready;
        std::lock_guard<raw::Mutex> lock(lock_);
        const State current = state();
        if (current == State::Pending && observer)
            observers_.push_back(std::move(observer));
        return current;
    }

    bool publish(T *value)
    {
        return settle(value, value ? State::Ready : State::Refused);
    }

    bool fail()
    {
        return settle(nullptr, State::Failed);
    }

private:
    bool settle(T *value, State settled)
    {
        std::vector<std::function<void()>> observers;
        {
            std::lock_guard<raw::Mutex> lock(lock_);
            if (state() != State::Pending)
                return false;
            value_ = value;
            state_.store(settled, std::memory_order_release);
            observers.swap(observers_);
        }
        for (auto &observer : observers)
            observer();
        return true;
    }

    mutable raw::Mutex lock_;
    std::atomic<State> state_{State::Pending};
    T *value_ = nullptr;
    std::vector<std::function<void()>> observers_;
};

}  // namespace raw
