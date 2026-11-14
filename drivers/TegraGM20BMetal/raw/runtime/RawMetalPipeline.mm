// SPDX-License-Identifier: MIT
#import "RawMetalPipeline.h"
#import "RawMetalRangeEncode.h"
#include <CoreFoundation/CoreFoundation.h>

namespace {

// Tape commands recorded since the worker was last handed a command buffer before it is handed again: each hand-off may
// wake the worker, and what was not handed is left for the commit to encode
constexpr uint32_t kHandOff = 24;

// The command buffer encoded ahead (the worker's alone): handed and not begun, or open on the device
struct Ahead {
    RMCommandBuffer *cb = nil;
    bool open = false;
    uint64_t moves = 0;  // texture moves (rmExcludingSubmissions) when it began
    const char *why = nullptr;
    size_t refused = 0;
};

Ahead &ahead()
{
    static Ahead *a = new Ahead;
    return *a;
}

uint32_t encodersBefore(RMCommandBuffer *cb, size_t position)
{
    uint32_t n = 0;
    for (size_t at = 0; at < position; ++at)
        n += RMCommandPlan::isEncoderBegin(cb->_plan.operation(at).op);
    return n;
}

// The encode ahead given back for `veto` (the first reason counted): the device's command buffer abandoned, and the
// command buffer encoded at its commit
void giveBack(uint32_t veto)
{
    Ahead &a = ahead();
    if (!a.cb)
        return;
    if (a.open) {
        rmRangeEncode().reset();
        gRM->committed.fetch_sub(1);
        a.cb->_pipelineAbandons++;
        a.cb->_pipelineGivenBack |= veto;
        RAW_DIAGNOSTIC(gRM->stats.pipelineAbandons[__builtin_ctz(veto ? veto : uint32_t(VetoFailed))]++);
    }
    a.cb->_pipelineDeclined.store(true, std::memory_order_relaxed);
    a.cb = nil;
    a.open = false;
}

}  // namespace

void rmPublishEncoder(RMCommandBuffer *cb)
{
    const size_t length = cb->_plan.size();
    cb->_published.store(uint64_t(cb->_endedAt) << 32 | length, std::memory_order_seq_cst);
    cb->_endedAt = cb->_encAt;
    cb->_handedCommands += uint32_t(length - cb->_encAt);
    if (cb->_handedCommands < kHandOff || cb->_pipelineVeto.load(std::memory_order_relaxed))
        return;
    cb->_handedCommands = 0;
    RAW_DIAGNOSTIC(gRM->stats.handoffs++);
    if (!cb->_handedOnce) {
        cb->_handedOnce = true;
        RAW_DIAGNOSTIC(gRM->stats.handoffWakes += gRM->submitAsleep.load(std::memory_order_relaxed));
        gRM->schedule.push({RMScheduleChange::Encoded, cb});
        return;
    }
    // the worker holds it already and reads _published before it sleeps: a wake only for a worker asleep
    if (!cb->_pipelineDeclined.load(std::memory_order_relaxed) && gRM->submitAsleep.load(std::memory_order_seq_cst)) {
        RAW_DIAGNOSTIC(gRM->stats.handoffWakes++);
        gRM->schedule.wake();
    }
}

void rmPipelineHanded(RMCommandBuffer *cb)
{
    Ahead &a = ahead();
    if (cb->_pipelineDeclined.load(std::memory_order_relaxed) || cb->_committed.load(std::memory_order_acquire))
        return;
    if (a.cb) {
        cb->_pipelineDeclined.store(true, std::memory_order_relaxed);
        return;
    }
    a.cb = cb;
    a.open = false;
}

bool rmPipelinePublished()
{
    const Ahead &a = ahead();
    if (!a.cb)
        return false;
    const size_t until = size_t(a.cb->_published.load(std::memory_order_seq_cst) >> 32);
    return until > (a.open ? rmRangeEncode().position() : 0);
}

bool rmPipelineStep()
{
    Ahead &a = ahead();
    RMCommandBuffer *cb = a.cb;
    if (!cb)
        return false;
    const uint64_t published = cb->_published.load(std::memory_order_acquire);
    if (const uint32_t veto = cb->_pipelineVeto.load(std::memory_order_acquire)) {
        giveBack(veto);
        return true;
    }
    if (a.open && gRM->moves.load(std::memory_order_acquire) != a.moves) {
        giveBack(VetoDevice);
        return true;
    }
    const size_t until = size_t(published >> 32);
    RMRangeEncode &encode = rmRangeEncode();
    if (a.open && encode.position() >= until)
        return false;
    if (!a.open && !until)
        return false;
    return rmSubmission([&] {
        if (!a.open) {
            // counted as committed, no purge takes an arena it encodes (raw_gpu.h purgeArenas)
            gRM->committed.fetch_add(1);
            a.moves = gRM->moves.load(std::memory_order_acquire);
            if (gRM->raw->purgedArenas() || !encode.open(cb, 0, SIZE_MAX, false, &a.why, &a.refused)) {
                encode.reset();
                gRM->committed.fetch_sub(1);
                giveBack(VetoDevice);
                return true;
            }
            a.open = true;
        }
        if (!encode.step(until))
            giveBack(encode.vetoed());
        return true;
    });
}

void rmPipelineYield(RMCommandBuffer *cb, size_t from, bool nativeRange)
{
    Ahead &a = ahead();
    if (a.cb && !(a.cb == cb && nativeRange && from == 0))
        giveBack(VetoOtherWork);
}

bool rmPipelineTake(RMCommandBuffer *cb, size_t from)
{
    Ahead &a = ahead();
    if (a.cb != cb || from)
        return false;
    if (!a.open) {
        giveBack(0);
        return false;
    }
    RMRangeEncode &encode = rmRangeEncode();
    if (const uint32_t veto = cb->_pipelineVeto.load(std::memory_order_acquire)) {
        giveBack(veto);
        return false;
    }
    if (gRM->moves.load(std::memory_order_acquire) != a.moves || !gRM->raw->channelCurrent(encode.generation())) {
        giveBack(VetoDevice);
        return false;
    }
#if SWITCH_RAW_DIAGNOSTICS
    cb->_pipelinedEncoders = encodersBefore(cb, encode.position());
#endif
    gRM->committed.fetch_sub(1);
    a.cb = nil;
    a.open = false;
    return true;
}

void rmPipelineOrphans()
{
    Ahead &a = ahead();
    if (a.cb && !a.cb->_committed.load(std::memory_order_acquire) && CFGetRetainCount((__bridge CFTypeRef)a.cb) == 1)
        giveBack(VetoOtherWork);
}

void rmPipelineSealed(RMCommandBuffer *cb, size_t to)
{
#if SWITCH_RAW_DIAGNOSTICS
    const uint32_t encoders = encodersBefore(cb, to);
    cb->_pipelineEncoders = encoders;
    gRM->stats.sealedEncoders += encoders;
    gRM->stats.pipelinedEncoders += cb->_pipelinedEncoders;
#else
    (void)cb;
    (void)to;
#endif
}
