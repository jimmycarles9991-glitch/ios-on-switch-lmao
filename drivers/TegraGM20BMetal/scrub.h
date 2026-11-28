// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace gm20b {

// A freed span of a VOL=1 write-combined chunk (Shared buffers' alone), counted in its chunk's used bytes until the copy
// engine has zeroed it (scrubs.cpp): wanted, then filled at the front of a command buffer, then scrubbing in the job
// `seqno` on channel `generation`
struct Scrub {
    uint32_t chunk = 0;
    uint64_t offset = 0, bytes = 0, va = 0;
    uint32_t seqno = 0;
    uint64_t generation = 0;
};

// What a scrub's retire finds: its job still on the GPU, its zeros in DRAM (the span goes back), or its fill to run
// again (its channel was lost or a kill covered the job, so it may never have run)
enum class ScrubFate {
    Waiting,
    Zeroed,
    Again
};

// The fence first, then the channel, then the kill record. A kill writes its range before it moves the fence past every
// seqno it gave out, so a record read after the fence sees any kill covering it. A reopen retires the generation before it
// releases the stand-in fence done() then reads as passed (Device::reopen), so a done() that read it is followed by a
// channel read that finds it lost.
template <class Done, class Current, class Killed> ScrubFate scrubFate(Done done, Current current, Killed killed)
{
    const bool finished = done();
    const bool live = current();
    if (live && !finished)
        return ScrubFate::Waiting;
    return live && !killed() ? ScrubFate::Zeroed : ScrubFate::Again;
}

}  // namespace gm20b
