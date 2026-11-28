// When a device gives back the GPU memory no resource holds (the driver's empty chunks, gm20b::Device::trim; its caller
// names what to release). Apple's driver keeps such memory in IOGPU's resource pools, which it can mark purgeable
// (IOGPUResourceSetPurgeable), and AGXMetal listens for DISPATCH_SOURCE_TYPE_MEMORYPRESSURE. Ours are kernel BOs mapped
// into the process and charged to its footprint as iokit_mapped until freed, so they go back on the events iOS's own
// purge and jetsam act on: memory pressure (the system's level and the process's own limit, which our memorystatus also
// warns a daemon of as it goes clean, before its demotion applies its inactive limit: kern_memorystatus.c
// memorystatus_dirty_set), and the app going to the background, before RunningBoard suspends it and memorystatus moves
// it to a band with a lower limit. A foreground app that pauses keeps its memory and pays no unpark at its next frame
// (owner, 25 Nov: no idle timer).
#pragma once
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <atomic>

class GpuMemoryRelease {
public:
    void start(void (*releaser)(void *), void *context)
    {
        release_ = releaser;
        context_ = context;
        queue_ =
            dispatch_queue_create("com.brunostjohn.switch.gpu-memory",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
        // The process's own limit warnings (libdispatch's private PROC_LIMIT bits) as well as the system's: WidgetRenderer's
        // warning came from nearing its own limit.
        pressure_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, kPressure | kProcLimit, queue_);
        if (!pressure_)
            pressure_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, kPressure, queue_);
        if (pressure_) {
            dispatch_source_set_event_handler(pressure_, ^{
                release();
            });
            dispatch_activate(pressure_);
        }
    }

    // The app went to the background (UIApplicationDidEnterBackgroundNotification, or an extension's host did:
    // NSExtensionHostDidEnterBackgroundNotification), on the thread that posts it: the memory goes back before this
    // returns, since the app may be suspended soon after. Once per transition: a second post before the app comes back
    // gives back nothing more.
    void backgrounded()
    {
        if (!queue_ || background_.exchange(true, std::memory_order_acq_rel))
            return;
        dispatch_sync_f(queue_, this, [](void *self) {
            static_cast<GpuMemoryRelease *>(self)->release();
        });
    }

    void foregrounded()
    {
        background_.store(false, std::memory_order_release);
    }

    void release()
    {
        if (release_)
            release_(context_);
    }

private:
    static constexpr uintptr_t kPressure = DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL;
    static constexpr uintptr_t kProcLimit = 0x10 | 0x20;  // DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN, _CRITICAL
    void (*release_)(void *) = nullptr;
    void *context_ = nullptr;
    dispatch_queue_t queue_ = nullptr;
    dispatch_source_t pressure_ = nullptr;
    std::atomic<bool> background_{false};
};
