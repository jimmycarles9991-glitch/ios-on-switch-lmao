// SPDX-License-Identifier: MIT
#pragma once
#include "follow.h"
#include "handoff_queue.h"
#include <pthread.h>
#include <functional>
#include <memory>

extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);

namespace gm20b {

// Serial executor over one follower pthread of its own with a known Mach thread port, so waitUntilScheduled can donate
// priority via OwnedWord across scheduled-handler execution with no ownerless gap and without spawning a priority-31
// root-queue thread under CPU saturation. Started once by its owner before any other thread can reach it, so enqueue
// takes no lock.
class OrderedExecutor {
public:
    OrderedExecutor() = default;

    ~OrderedExecutor()
    {
        stop();
    }

    OrderedExecutor(const OrderedExecutor &) = delete;
    OrderedExecutor &operator=(const OrderedExecutor &) = delete;

    // False: no thread could be made (pthread_create's error in `err`), and nothing may be enqueued
    bool start(int &err)
    {
        auto state = std::make_shared<State>();
        auto *arg = new std::shared_ptr<State>(state);
        pthread_attr_t attr;
        followers().initAttr(&attr);
        pthread_t thread = nullptr;
        err = pthread_create(&thread, &attr, &OrderedExecutor::run, arg);
        pthread_attr_destroy(&attr);
        if (err != 0) {
            delete arg;
            return false;
        }
        // nothing joins it: the thread holds its own state, so it may outlive this executor (one destroyed from inside its
        // own handler, or with items queued) and runs what was queued before the stop
        pthread_detach(thread);
        owner_ = uint32_t(pthread_mach_thread_np(thread));
        state_ = std::move(state);
        return true;
    }

    uint32_t ownerPort() const
    {
        return owner_;
    }

    void enqueue(std::function<void()> fn)
    {
        if (fn)
            state_->queue.push(std::move(fn));
    }

    void stop()
    {
        if (std::shared_ptr<State> state = std::move(state_))
            state->queue.push(std::function<void()>{});
        owner_ = 0;
    }

private:
    struct State {
        HandoffQueue<std::function<void()>> queue;
    };

    static void *run(void *arg)
    {
        std::unique_ptr<std::shared_ptr<State>> holder(static_cast<std::shared_ptr<State> *>(arg));
        std::shared_ptr<State> state = std::move(*holder);
        holder.reset();
        pthread_setname_np("com.brunostjohn.switch.gm20b.handlers");
        if (!followers().isFixed())
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        followers().join();
        for (;;) {
            std::function<void()> &next = state->queue.front();
            // the item's run and its captures' release (a command buffer's last, its handlers' blocks) both inside it: a
            // thread that lives as long as its queue drains no pool of its own otherwise
            void *pool = objc_autoreleasePoolPush();
            std::function<void()> fn = std::move(next);
            state->queue.pop();
            const bool stopped = !fn;
            if (fn)
                fn();
            fn = nullptr;
            objc_autoreleasePoolPop(pool);
            if (stopped)
                return nullptr;
        }
    }

    std::shared_ptr<State> state_;
    uint32_t owner_ = 0;
};

}  // namespace gm20b
