// When a device gives back the GPU memory no resource holds (the driver's empty chunks, gm20b::Device::trim; its caller
// names what to release, so nothing here is m2v's). Apple's driver keeps such memory in IOGPU's resource pools, which it can mark purgeable (IOGPUResourceSetPurgeable)
// so that it stops counting in the process's footprint, and AGXMetal listens for DISPATCH_SOURCE_TYPE_MEMORYPRESSURE. Ours
// are kernel BOs mapped into the process and charged to its footprint as iokit_mapped until freed, so they go back on the
// same events, and once the GPU has had nothing to do for a second: a widget or a snapshot service that stopped drawing
// would otherwise carry them into its lower inactive limit, past which jetsam kills it at once, with no warning to answer.
// The render server's go back on memory pressure alone (IdleDeadline::pressureOnly): what an idle second gave back, its
// render thread made again at the next frame (22 Nov, boot B: an 8 MiB heap's 4.4 ms of zero-fill after every pause).
#pragma once
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdlib.h>
#include <atomic>

class GpuMemoryRelease {
public:
    static constexpr int64_t kIdleNs = 1000 * int64_t(NSEC_PER_MSEC);

    // Memory pressure alone: the caller's own wait keeps the idle deadline (IdleDeadline) and calls releaseSoon() at it
    void startOnPressure(void (*releaser)(void *), void *context)
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

    // Memory pressure, and the idle deadline on a timer of its own that drained() moves (the m2v bridge's).
    // inFlight: the caller's command buffers committed and not finished. With it, the idle deadline gives nothing back
    // while one is in flight (a GPU busy past it without a drain is not idle) and the next drain sets it again; memory
    // pressure still gives back at once.
    void start(void (*releaser)(void *), void *context, const std::atomic<uint32_t> *inFlight = nullptr)
    {
        startOnPressure(releaser, context);
        inFlight_ = inFlight;
        idle_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
        dispatch_source_set_timer(idle_, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, kLeeway);
        dispatch_source_set_event_handler(idle_, ^{
            // Disarmed before the drain is read: a drain after this arms the timer itself
            armed_.store(false);
            if (dispatch_time(DISPATCH_TIME_NOW, 0) < dispatch_time(lastDrain_.load(), kIdleNs)) {
                arm();
                return;
            }
            if (!inFlight_ || !inFlight_->load(std::memory_order_acquire))
                release();
        });
        dispatch_activate(idle_);
        // A process that opens the device and draws nothing gives back the room its first queue kept.
        drained();
    }

    // The GPU finished the last command buffer in flight: the memory goes once no other completed for the idle time. Each
    // drain moves the deadline, so a process drawing frames never reaches it. A drain only stamps its time; the timer,
    // armed by the drain that found it idle, moves itself to the latest drain's deadline when it fires. Setting a
    // dispatch timer allocates its configuration and wakes the timer's queue, which a drain at every frame would pay.
    void drained()
    {
        if (!idle_)
            return;
        lastDrain_.store(dispatch_time(DISPATCH_TIME_NOW, 0));
        if (!armed_.load())
            arm();
    }

    void release()
    {
        if (release_)
            release_(context_);
    }

    // The release on its own queue, for a thread that must not run it
    void releaseSoon()
    {
        dispatch_async_f(queue_, this, [](void *self) {
            static_cast<GpuMemoryRelease *>(self)->release();
        });
    }

private:
    void arm()
    {
        if (!armed_.exchange(true))
            dispatch_source_set_timer(idle_, dispatch_time(lastDrain_.load(), kIdleNs), DISPATCH_TIME_FOREVER, kLeeway);
    }

    static constexpr uint64_t kLeeway = 100 * NSEC_PER_MSEC;
    static constexpr uintptr_t kPressure = DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL;
    static constexpr uintptr_t kProcLimit = 0x10 | 0x20;  // DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN, _CRITICAL
    void (*release_)(void *) = nullptr;
    void *context_ = nullptr;
    const std::atomic<uint32_t> *inFlight_ = nullptr;
    dispatch_queue_t queue_ = nullptr;
    dispatch_source_t idle_ = nullptr, pressure_ = nullptr;
    // Atomics, not ownership: the completion thread drains while the timer's queue reads the drain and disarms.
    // Sequentially consistent: a drain's stamp precedes its read of armed_, the handler's disarm precedes its read of
    // the stamp, so one of the two always sees the other and the latest drain's deadline is never lost.
    std::atomic<dispatch_time_t> lastDrain_{0};
    std::atomic<bool> armed_{false};
};

// The idle deadline a thread's own wait carries (the raw completion thread's, started on pressure alone): a drain
// stamps its time; the waiting thread, with nothing in flight, sleeps no later than due() and claims the release there.
// A process drawing frames wakes that thread for each completion anyway and never reaches it; work in flight holds it
// until the next drain stamps it again. Times are CLOCK_UPTIME_RAW nanoseconds.
class IdleDeadline {
public:
    // The render server's: no drain sets a deadline, so memory pressure alone gives back
    void pressureOnly()
    {
        pressureOnly_ = true;
    }

    void drained(uint64_t now)
    {
        if (!pressureOnly_)
            drainedAt_.store(now, std::memory_order_release);
    }

    // When the release is due with `inFlight` ranges in flight: 0 for none
    uint64_t due(uint32_t inFlight) const
    {
        const uint64_t at = drainedAt_.load(std::memory_order_acquire);
        return at && !inFlight ? at + uint64_t(GpuMemoryRelease::kIdleNs) : 0;
    }

    // The waiting thread's, past a due() it read: true once for that drain, false where a later drain moved it
    bool claim(uint64_t due)
    {
        uint64_t at = due - uint64_t(GpuMemoryRelease::kIdleNs);
        return drainedAt_.compare_exchange_strong(at, 0, std::memory_order_acq_rel);
    }

private:
    // Atomic, not the waiting thread's own: the m2v fallback's completion handlers drain on their threads
    std::atomic<uint64_t> drainedAt_{0};
    // Set at open, before the threads that drain are made
    bool pressureOnly_ = false;
};
