// When a device gives back the GPU memory no resource holds (nmb_device_release_idle_memory: m2v's empty blocks and
// spares; the raw path's empty chunks, raw::Device::trim). Apple's driver keeps such memory in IOGPU's resource pools, which it can mark purgeable (IOGPUResourceSetPurgeable)
// so that it stops counting in the process's footprint, and AGXMetal listens for DISPATCH_SOURCE_TYPE_MEMORYPRESSURE. Ours
// are kernel BOs mapped into the process and charged to its footprint as iokit_mapped until freed, so they go back on the
// same events, and once the GPU has had nothing to do for a second: a widget or a snapshot service that stopped drawing
// would otherwise carry them into its lower inactive limit, past which jetsam kills it at once, with no warning to answer.
//
// NMB_IDLE_RELEASE_MS sets that idle time (default 1000); 0 releases on memory pressure alone, as the render server does: its
// blocks would otherwise be made again on its render thread at the first frame after every still screen.
#pragma once
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdlib.h>
#include <atomic>
#include "bridge/NativeMetalObjects.h"

class GpuMemoryRelease {
public:
    void start(NmbDevice *device)
    {
        start(
            [](void *d) {
                nmb_device_release_idle_memory(static_cast<NmbDevice *>(d));
            },
            device);
    }

    // inFlight: the caller's command buffers committed and not finished. With it, the idle deadline gives nothing back
    // while one is in flight (a GPU busy past it without a drain is not idle) and the next drain sets it again; memory
    // pressure still gives back at once.
    void start(void (*releaser)(void *), void *context, const std::atomic<uint32_t> *inFlight = nullptr)
    {
        release_ = releaser;
        context_ = context;
        inFlight_ = inFlight;
        queue_ =
            dispatch_queue_create("com.brunostjohn.switch.gpu-memory",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
        const char *ms = getenv("NMB_IDLE_RELEASE_MS");
        idleNs_ = (ms ? strtoll(ms, nullptr, 10) : kIdleMs) * int64_t(NSEC_PER_MSEC);
        if (idleNs_ > 0) {
            idle_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
            dispatch_source_set_timer(idle_, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, kLeeway);
            dispatch_source_set_event_handler(idle_, ^{
                dispatch_source_set_timer(idle_, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, kLeeway);
                if (!inFlight_ || !inFlight_->load(std::memory_order_acquire))
                    release();
            });
            dispatch_activate(idle_);
        }
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
        // A process that opens the device and draws nothing gives back the room its first queue kept.
        drained();
    }

    // The GPU finished the last command buffer in flight: the memory goes once no other completed for the idle time. Each
    // drain moves the deadline, so a process drawing frames never reaches it.
    void drained()
    {
        if (idle_)
            dispatch_source_set_timer(idle_, dispatch_time(DISPATCH_TIME_NOW, idleNs_), DISPATCH_TIME_FOREVER, kLeeway);
    }

    void release()
    {
        if (release_)
            release_(context_);
    }

private:
    static constexpr int64_t kIdleMs = 1000;
    static constexpr uint64_t kLeeway = 100 * NSEC_PER_MSEC;
    int64_t idleNs_ = 0;
    static constexpr uintptr_t kPressure = DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL;
    static constexpr uintptr_t kProcLimit = 0x10 | 0x20;  // DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN, _CRITICAL
    void (*release_)(void *) = nullptr;
    void *context_ = nullptr;
    const std::atomic<uint32_t> *inFlight_ = nullptr;
    dispatch_queue_t queue_ = nullptr;
    dispatch_source_t idle_ = nullptr, pressure_ = nullptr;
};
