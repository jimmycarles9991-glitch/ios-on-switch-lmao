// SPDX-License-Identifier: MIT
// When waitUntilScheduled returns (TegraGM20BMetalCommandBuffer.mm), and which waits of a queue's head let the command buffers
// committed behind it count as scheduled before their work is submitted (TegraGM20BMetalSchedule.mm).
#pragma once
#include <cstdint>

namespace gm20b {

constexpr uint32_t kEarlySchedulePasses = 4;

constexpr bool earlySchedule(bool compositor, bool workerBusy, uint32_t remainingPasses, bool multiRange)
{
    return compositor && (workerBusy || multiRange || remainingPasses >= kEarlySchedulePasses);
}

enum class HeadWait {
    event,     // a shared event value nothing reached yet
    promised,  // a shared event value a submitted command buffer publishes as its work completes (event_timeline.h)
    compile,   // a pipeline being made at the committers' priority, which waits for nothing its caller does
    signal,    // its own earlier work, before a signal or wait after it
    order,     // the other driver's work, ordered on the CPU
};

// A queue whose head waits on a shared event holds: the surfaces its command buffers write become the kernel's promised
// writes (TegraGM20BMetalSchedule.mm promiseWrites, TegraGM20BMethodWriterPromise), which a present or another process's job
// waits for, as Apple's kernel knows a committed command buffer's writes, and the kernel holds the wait's bound on them
// (TegraGM20BPromiseArm).
constexpr bool holds(HeadWait wait)
{
    return wait == HeadWait::event;
}

// `scheduled`: its scheduled handlers ran; `failed`: no more of it will be submitted. A held command buffer is neither:
// Metal's waitUntilScheduled returns at the signal or at the failure 5 s on (an M4 Max), and a thread that commits the
// signaller after its own wait waits that long there too.
constexpr bool scheduledWaitReturns(bool scheduled, bool failed)
{
    return scheduled || failed;
}

}  // namespace gm20b
