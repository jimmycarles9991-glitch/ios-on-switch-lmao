// SPDX-License-Identifier: MIT
// A GPU for the kernel model (raw_kernel_model.h) that takes a set time for each job (setPerJob), in submission order, and
// stamps when each job was submitted: work in flight for a measure of the submitting side. A WaitFence waits for it.
#pragma once
#include "raw_kernel_model.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace timed_gpu {

using Clock = std::chrono::steady_clock;

class Gpu {
public:
    Gpu()
    {
        using kernel_model::model;
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        first_ = next_ = model.jobs.size();
        model.jobAdded = [this] {
            std::lock_guard<std::mutex> g(lock_);
            submittedAt_.push_back(Clock::now());
            wake_.notify_all();
        };
        model.blockUntil = [this](uint32_t channel, uint32_t seqno) {
            std::unique_lock<std::mutex> g(lock_);
            return wake_.wait_for(g, std::chrono::seconds(30), [&] {
                return int32_t(kernel_model::model.fence(channel - 1).completed - seqno) >= 0;
            });
        };
        thread_ = std::thread([this] {
            loop();
        });
    }

    ~Gpu()
    {
        {
            std::lock_guard<std::mutex> g(lock_);
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.jobAdded = nullptr;
        kernel_model::model.blockUntil = nullptr;
    }

    void setPerJob(std::chrono::microseconds perJob)
    {
        std::lock_guard<std::mutex> g(lock_);
        perJob_ = perJob;
    }

    // Jobs submitted since the GPU started
    size_t submitted()
    {
        std::lock_guard<std::mutex> g(lock_);
        return submittedAt_.size();
    }

    // When job `k` (counted from the GPU's start) was submitted
    Clock::time_point submittedAt(size_t k)
    {
        std::lock_guard<std::mutex> g(lock_);
        return k < submittedAt_.size() ? submittedAt_[k] : Clock::time_point{};
    }

private:
    void loop()
    {
        using kernel_model::model;
        for (;;) {
            size_t at;
            std::chrono::microseconds perJob;
            {
                std::unique_lock<std::mutex> g(lock_);
                wake_.wait(g, [&] {
                    return stopping_ || next_ - first_ < submittedAt_.size();
                });
                if (stopping_)
                    return;
                at = next_;
                perJob = perJob_;
            }
            std::this_thread::sleep_for(perJob);
            kernel_model::Job job;
            {
                std::lock_guard<std::mutex> l(kernel_model::callLock);
                job = model.jobs[at];
            }
            {
                std::lock_guard<std::mutex> g(lock_);
                __atomic_store_n(&model.fence(job.channel - 1).completed, job.seqno, __ATOMIC_RELEASE);
                ++next_;
            }
            wake_.notify_all();
        }
    }

    std::chrono::microseconds perJob_{0};
    std::mutex lock_;
    std::condition_variable wake_;
    size_t first_ = 0, next_ = 0;
    std::vector<Clock::time_point> submittedAt_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace timed_gpu
