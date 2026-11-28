// SPDX-License-Identifier: MIT
// One sync of the store and the request log (compiler_store.h, compiler_log.h persist) a while after the first publish
// that follows the last sync, at the background class: a burst of compiles costs one sync, and nothing that answers a
// client waits for it. A crash loses at most the entries made since, which their next ask makes again.
#pragma once
#include <dispatch/dispatch.h>
#include <atomic>
#include <functional>
#include <utility>

namespace switch_compiler {

class Persister {
public:
    explicit Persister(std::function<void()> persist) : persist_(std::move(persist))
    {}

    Persister(const Persister &) = delete;
    Persister &operator=(const Persister &) = delete;

    void soon()
    {
        if (armed_.exchange(true, std::memory_order_acq_rel))
            return;
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, kDelayNs), queue(), this, fire);
    }

private:
    static constexpr int64_t kDelayNs = 2 * int64_t(NSEC_PER_SEC);

    std::function<void()> persist_;
    std::atomic<bool> armed_{false};

    // Serial, so two syncs never overlap
    static dispatch_queue_t queue()
    {
        static dispatch_queue_t q = dispatch_queue_create(
            "com.brunostjohn.switch.shaders.persist",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0));
        return q;
    }

    // Disarmed before it syncs, so a publish during the sync asks for the next one
    static void fire(void *self)
    {
        auto *persister = static_cast<Persister *>(self);
        persister->armed_.store(false, std::memory_order_release);
        persister->persist_();
    }
};

}  // namespace switch_compiler
