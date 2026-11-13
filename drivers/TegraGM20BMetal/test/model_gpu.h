// SPDX-License-Identifier: MIT
// The GPU of a test over the kernel model (raw_kernel_model.h): a thread that runs each job submitted from its start on
// the predication model (predication_model.h) over the model's memory, in submission order, and only then completes it, as
// the fence interrupt follows the GPU. Held, it leaves what it has not begun in flight.
#pragma once
#include "raw_kernel_model.h"
#include "predication_model.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace model_gpu {

class Gpu {
public:
    using Launch = std::function<void(uint32_t *qmd, predication_model::Gpu &)>;

    explicit Gpu(Launch launch)
    {
        using kernel_model::model;
        gpu_.map = [](uint64_t va) {
            std::lock_guard<std::mutex> l(kernel_model::callLock);
            return kernel_model::model.host(va);
        };
        gpu_.onLaunch = [this, launch](uint32_t pcas) {
            launch(reinterpret_cast<uint32_t *>(gpu_.at(uint64_t(pcas) << 8)), gpu_);
        };
        {
            std::lock_guard<std::mutex> l(kernel_model::callLock);
            first_ = next_ = submitted_ = model.jobs.size();
            model.jobAdded = [this] {
                std::lock_guard<std::mutex> g(lock_);
                ++submitted_;
                wake_.notify_all();
            };
            model.blockUntil = [this](uint32_t channel, uint32_t seqno) {
                std::unique_lock<std::mutex> g(lock_);
                return wake_.wait_for(g, std::chrono::seconds(30), [&] {
                    return int32_t(kernel_model::model.fence(channel - 1).completed - seqno) >= 0;
                });
            };
        }
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

    void hold(bool held)
    {
        {
            std::lock_guard<std::mutex> g(lock_);
            held_ = held;
        }
        wake_.notify_all();
    }

    // The jobs submitted since the GPU started, and how many of them ran
    size_t submitted()
    {
        std::lock_guard<std::mutex> g(lock_);
        return submitted_ - first_;
    }

    size_t ran()
    {
        std::lock_guard<std::mutex> g(lock_);
        return next_ - first_;
    }

    // Whether `count` jobs are submitted within `timeout`
    bool submittedBy(size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> g(lock_);
        return wake_.wait_for(g, timeout, [&] {
            return submitted_ - first_ >= count;
        });
    }

    // GP entries of the jobs run so far that their fetcher reads without prefetch: copies of a laid-down loop
    size_t noPrefetchEntries()
    {
        std::lock_guard<std::mutex> g(lock_);
        return noPrefetch_;
    }

private:
    void loop()
    {
        using kernel_model::model;
        for (;;) {
            size_t at;
            {
                std::unique_lock<std::mutex> g(lock_);
                wake_.wait(g, [&] {
                    return stopping_ || (!held_ && next_ < submitted_);
                });
                if (stopping_)
                    return;
                at = next_;
            }
            kernel_model::Job job;
            {
                std::lock_guard<std::mutex> l(kernel_model::callLock);
                job = model.jobs[at];
            }
            size_t noPrefetch = 0;
            for (const TegraGM20BAbiPush &push : job.pushes) {
                noPrefetch += (push.flags & TegraGM20BPushNoPrefetch) != 0;
                std::vector<uint32_t> words(push.dwords);
                memcpy(words.data(), gpu_.at(push.va), words.size() * 4);
                gpu_.run(words.data(), words.size(), true);
            }
            {
                std::lock_guard<std::mutex> g(lock_);
                __atomic_store_n(&model.fence(job.channel - 1).completed, job.seqno, __ATOMIC_RELEASE);
                noPrefetch_ += noPrefetch;
                ++next_;
            }
            wake_.notify_all();
        }
    }

    predication_model::Gpu gpu_{0};
    std::mutex lock_;
    std::condition_variable wake_;
    size_t first_ = 0, next_ = 0, submitted_ = 0, noPrefetch_ = 0;
    bool held_ = false, stopping_ = false;
    std::thread thread_;
};

}  // namespace model_gpu
