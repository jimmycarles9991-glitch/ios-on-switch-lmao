// SPDX-License-Identifier: MIT
// When waitUntilScheduled returns (RawMetalCommandBuffer.mm), and which waits of a queue's head let the command buffers
// committed behind it count as scheduled before their work is submitted (RawMetalSchedule.mm).
#pragma once

namespace raw {

enum class HeadWait {
    event,     // a shared event value nothing reached yet
    promised,  // a shared event value a submitted command buffer publishes as its work completes (raw_event_timeline.h)
    compile,   // a pipeline being made at the committers' priority, which waits for nothing its caller does
    signal,    // its own earlier work, before a signal or wait after it
    order,     // the other driver's work, ordered on the CPU
};

// The kernel orders a present only after the writers it already has, so a command buffer counts as scheduled before its
// work is submitted only where waiting could deadlock: behind a shared event, which the waiting thread may be about to
// commit the signaller of. Even then the surfaces it writes are first the kernel's promised writes (RawMetalSchedule.mm
// promiseWrites, TegraGM20BMethodWriterPromise), so a present of one still waits for its job.
constexpr bool holds(HeadWait wait)
{
    return wait == HeadWait::event;
}

// `scheduled`: its scheduled handlers ran; `held`: its queue's head waits on a shared event; `failed`: no more of it will
// be submitted
constexpr bool scheduledWaitReturns(bool scheduled, bool held, bool failed)
{
    return scheduled || held || failed;
}

}  // namespace raw
