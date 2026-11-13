// SPDX-License-Identifier: MIT
#pragma once
#include "TegraGM20BABI.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace raw {
class SharedEvents;

class SharedEventLease {
public:
    ~SharedEventLease();

    uint64_t handle() const
    {
        return handle_;
    }

    std::shared_ptr<SharedEvents> owner() const
    {
        return owner_.lock();
    }

private:
    friend class SharedEvents;

    SharedEventLease(std::shared_ptr<SharedEvents> owner, uint64_t handle) : owner_(owner), handle_(handle)
    {}

    std::weak_ptr<SharedEvents> owner_;
    uint64_t handle_;
};

class SharedEventWatch {
public:
    ~SharedEventWatch();

private:
    friend class SharedEvents;

    SharedEventWatch(std::shared_ptr<SharedEventLease> event, uint64_t cookie)
        : event_(std::move(event)), cookie_(cookie)
    {}

    SharedEventWatch(std::shared_ptr<SharedEvents> owner, uint64_t cookie) : owner_(owner), cookie_(cookie)
    {}

    std::shared_ptr<SharedEventLease> event_;
    std::weak_ptr<SharedEvents> owner_;
    uint64_t cookie_;
};

class SharedEventCompletion {
public:
    int result() const
    {
        return result_->load(std::memory_order_acquire);
    }

private:
    friend class SharedEvents;

    explicit SharedEventCompletion(std::shared_ptr<std::atomic<int>> result) : result_(std::move(result))
    {}

    std::shared_ptr<std::atomic<int>> result_;
    std::shared_ptr<SharedEventWatch> watch_;
};

class ResourceReadyCompletion {
public:
    int result() const
    {
        return result_->load(std::memory_order_acquire);
    }

private:
    friend class SharedEvents;

    explicit ResourceReadyCompletion(std::shared_ptr<std::atomic<int>> result) : result_(std::move(result))
    {}

    std::shared_ptr<std::atomic<int>> result_;
    std::shared_ptr<SharedEventWatch> watch_;
};

class SharedEvents : public std::enable_shared_from_this<SharedEvents> {
public:
    static std::shared_ptr<SharedEvents> create(uint32_t connection, bool tunnel, uint32_t selector);
    ~SharedEvents();
    std::shared_ptr<SharedEventLease> import(uint32_t port);
    int probe(const SharedEventLease &event, uint64_t value);
    std::shared_ptr<SharedEventWatch> observe(std::shared_ptr<SharedEventLease> event, uint64_t value,
                                              std::function<void(int)> ready, int &result, bool terminal = false);
    std::shared_ptr<SharedEventCompletion> completion(std::shared_ptr<SharedEventLease> event, uint64_t value,
                                                      std::function<void()> ready, int &result);
    std::shared_ptr<ResourceReadyCompletion> resources(const std::vector<TegraGM20BAbiSurfaceUse> &uses,
                                                       std::function<void()> ready, int &result);
    void shutdown();

private:
    friend class SharedEventLease;
    friend class SharedEventWatch;
    struct State;
    SharedEvents(uint32_t connection, bool tunnel, uint32_t selector);
    void release(uint64_t handle);
    void cancel(uint64_t handle, uint64_t cookie);
    void delivered(uint64_t cookie, int result);
    static void notification(void *context, int result, void *cookie);
    std::unique_ptr<State> state_;
};

struct JobEvents {
    const TegraGM20BAbiEventUse *waits = nullptr, *signals = nullptr;
    uint32_t waitCount = 0, signalCount = 0;
    // One per signal (null: none): the payload the job's pushes release into the event's word for it, 0 where its job's end
    // signals it (TegraGM20BJobABI.h method 40)
    const uint32_t *releases = nullptr;

    bool empty() const
    {
        return !waitCount && !signalCount;
    }
};
}
