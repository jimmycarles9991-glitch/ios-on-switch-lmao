// SPDX-License-Identifier: MIT
#pragma once
#include "raw_lock.h"
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>
#include "raw_compute_key.h"
#include "raw_variant.h"
#include "raw_dispatch_regions.h"

namespace raw {

inline bool operator<(const BoundTexture &a, const BoundTexture &b)
{
    return std::tie(a.slot, a.pixelFormat, a.textureType, a.levels, a.arrayLength, a.swizzle[0], a.swizzle[1],
                    a.swizzle[2], a.swizzle[3]) < std::tie(b.slot, b.pixelFormat, b.textureType, b.levels,
                                                           b.arrayLength, b.swizzle[0], b.swizzle[1], b.swizzle[2],
                                                           b.swizzle[3]);
}

inline uint32_t computeLodBits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline bool operator<(const BoundSampler &a, const BoundSampler &b)
{
    return std::make_tuple(a.slot, a.minFilter, a.magFilter, a.mipFilter, a.sAddress, a.tAddress, a.rAddress,
                           a.maxAnisotropy, a.compare, a.border, computeLodBits(a.lodMin), computeLodBits(a.lodMax),
                           a.normalized, a.lodAverage) <
           std::make_tuple(b.slot, b.minFilter, b.magFilter, b.mipFilter, b.sAddress, b.tAddress, b.rAddress,
                           b.maxAnisotropy, b.compare, b.border, computeLodBits(b.lodMin), computeLodBits(b.lodMax),
                           b.normalized, b.lodAverage);
}

struct ComputeBindingText {
    std::string memory, textures, samplers;
};

struct ComputeSpecialization {
    std::array<uint32_t, 3> threads = {};
    bool regionMode = false;
    std::vector<std::pair<uint32_t, uint32_t>> memory;
    std::vector<BoundTexture> textures;
    std::vector<BoundSampler> samplers;

    bool operator<(const ComputeSpecialization &b) const
    {
        return std::tie(threads, regionMode, memory, textures, samplers) <
               std::tie(b.threads, b.regionMode, b.memory, b.textures, b.samplers);
    }

    bool operator==(const ComputeSpecialization &b) const
    {
        return !(*this < b) && !(b < *this);
    }

    bool bindTexture(uint32_t slot, const BoundTexture *value)
    {
        return bind(textures, slot, value);
    }

    bool bindSampler(uint32_t slot, const BoundSampler *value)
    {
        return bind(samplers, slot, value);
    }

    bool bindMemory(uint32_t slot, uint32_t bytes)
    {
        const auto at = std::lower_bound(memory.begin(), memory.end(), slot, [](const auto &v, uint32_t i) {
            return v.first < i;
        });
        if (at != memory.end() && at->first == slot) {
            if (at->second == bytes)
                return false;
            if (bytes)
                at->second = bytes;
            else
                memory.erase(at);
        } else {
            if (!bytes)
                return false;
            memory.insert(at, {slot, bytes});
        }
        return true;
    }

    ComputeBindingText text() const
    {
        ComputeBindingText result;
        for (const auto &[slot, bytes] : memory)
            result.memory += (result.memory.empty() ? "" : ",") + std::to_string(slot) + ":" + std::to_string(bytes);
        result.textures = computeTextures(textures.data(), textures.size());
        result.samplers = computeSamplers(samplers.data(), samplers.size());
        return result;
    }

private:
    template <typename T> static bool bind(std::vector<T> &values, uint32_t slot, const T *value)
    {
        const auto at = std::lower_bound(values.begin(), values.end(), slot, [](const T &v, uint32_t i) {
            return v.slot < i;
        });
        if (at != values.end() && at->slot == slot) {
            if (!value)
                values.erase(at);
            else if (*at < *value || *value < *at)
                *at = *value;
            else
                return false;
        } else {
            if (!value)
                return false;
            values.insert(at, *value);
        }
        return true;
    }
};

template <typename Record, typename T> class ComputeKernelBuild {
public:
    // Gone: no compiler made it, and none will (the dispatches that need it fail)
    enum class Outcome {
        Made,
        Refused,
        Failed,
        Gone
    };

    struct Result {
        const T *state = nullptr;
        bool retry = false;
        uint64_t retryAtUs = 0;
        bool gone = false;
    };

    template <typename Clock, typename Produce, typename Place> Result make(Clock now, Produce produce, Place place)
    {
        if (state_ || refused_ || gone_)
            return {state_, false, 0, gone_};
        if (retryAtUs_ && now() < retryAtUs_)
            return {nullptr, true, retryAtUs_};
        if (!record_) {
            auto record = std::make_unique<Record>();
            const auto outcome = produce(*record);
            if (outcome == Outcome::Refused || outcome == Outcome::Gone) {
                (outcome == Outcome::Gone ? gone_ : refused_) = true;
                return {nullptr, false, 0, gone_};
            }
            if (outcome == Outcome::Made)
                record_ = std::move(record);
        }
        if (record_ && (state_ = place(*record_)))
            return {state_, false, 0};
        const uint64_t failedUs = now();
        const uint64_t delayUs = delay_.next();
        retryAtUs_ = failedUs > UINT64_MAX - delayUs ? UINT64_MAX : failedUs + delayUs;
        return {nullptr, true, retryAtUs_};
    }

private:
    // placeCompute keys placed programs by this record's address, including across placement retries.
    std::unique_ptr<Record> record_;
    const T *state_ = nullptr;
    uint64_t retryAtUs_ = 0;
    RetryDelay delay_;
    bool refused_ = false, gone_ = false;
};

template <typename T> struct ComputeKernelResult {
    const T *state = nullptr;
    bool retry = false;
    bool deferredRetry = false;
    bool gone = false;  // no compiler made it: the command buffer that needs it fails
};

template <typename T> class ComputeKernelJob {
public:
    using State = typename Variant<const T>::State;

    State state() const
    {
        const auto *result = ready_.value();
        return !result ? State::Pending : result->state ? State::Ready : result->gone ? State::Failed : State::Refused;
    }

    const T *value() const
    {
        const auto *result = ready_.value();
        return result ? result->state : nullptr;
    }

    bool retryable() const
    {
        const auto *result = ready_.value();
        return result && result->retry && (!result->deferredRetry || retryReady_.load(std::memory_order_acquire));
    }

    void allowRetry()
    {
        retryReady_.store(true, std::memory_order_release);
    }

    State observe(std::function<void()> observer)
    {
        ready_.observe(std::move(observer));
        return state();
    }

    void publish(ComputeKernelResult<T> result)
    {
        if (claimed_.test_and_set(std::memory_order_relaxed))
            return;
        result_ = result;
        ready_.publish(&result_);
    }

    struct Run {
        std::shared_ptr<ComputeKernelJob> job;
        ComputeKernelResult<T> result{nullptr, true};

        ~Run()
        {
            job->publish(result);
        }
    };

private:
    ComputeKernelResult<T> result_;
    std::atomic<bool> retryReady_{false};
    Variant<ComputeKernelResult<T>> ready_;
    std::atomic_flag claimed_ = ATOMIC_FLAG_INIT;
};

template <typename T> class ComputeKernelCache {
public:
    using Job = ComputeKernelJob<T>;

    template <typename Start> std::shared_ptr<Job> request(const ComputeSpecialization &key, Start &&start)
    {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<raw::Mutex> hold(lock_);
            const auto at = entries_.find(key);
            if (at != entries_.end() && !at->second->retryable())
                return at->second;
            job = std::make_shared<Job>();
            entries_[key] = job;
        }

        struct Launch {
            std::shared_ptr<Job> job;
            bool started = false;

            ~Launch()
            {
                if (!started)
                    job->publish({nullptr, true});
            }
        } launch{job};

        start(job);
        launch.started = true;
        return job;
    }

private:
    raw::Mutex lock_;
    std::map<ComputeSpecialization, std::shared_ptr<Job>> entries_;
};

template <typename T> class ComputeKernelSet {
public:
    using Job = ComputeKernelJob<T>;
    using State = typename Job::State;
    std::array<std::shared_ptr<Job>, 8> jobs{};
    uint8_t count = 0;

    State state() const
    {
        bool pending = false, refused = false;
        for (uint8_t i = 0; i < count; ++i) {
            const auto result = jobs[i]->state();
            if (result == State::Failed)
                return result;
            refused |= result == State::Refused;
            pending |= result == State::Pending;
        }
        return refused ? State::Refused : pending ? State::Pending : State::Ready;
    }

    bool retryable() const
    {
        for (uint8_t i = 0; i < count; ++i)
            if (jobs[i]->retryable())
                return true;
        return false;
    }

    State observe(const std::function<void()> &observer)
    {
        if (observer)
            for (uint8_t i = 0; i < count; ++i)
                jobs[i]->observe(observer);
        return state();
    }
};

}  // namespace raw
