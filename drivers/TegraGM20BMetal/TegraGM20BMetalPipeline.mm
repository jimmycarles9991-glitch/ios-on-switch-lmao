// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalPipeline.h"
#import "TegraGM20BMetalRangeEncode.h"
#include <CoreFoundation/CoreFoundation.h>

namespace {

// Tape commands recorded since the worker was last handed a command buffer before it is handed again: each hand-off may
// wake the worker, and what was not handed is left for the commit to encode
constexpr uint32_t kHandOff = 24;

// The command buffer encoded ahead (the worker's alone): handed and not begun, or open on the device. The worker gives it
// back or takes it before it finishes that command buffer (gm20bPipelineYield, gm20bPipelineFailed), and before it runs a move
// of a texture's memory (gm20bRunOnSubmitWorker), so no encode ahead spans one: once its completion is sent, nothing of it
// is the worker's, and the counts giveBack wrote on it are final.
struct Ahead {
    SwitchMetalCommandBuffer *cb = nil;
    bool open = false;
    const char *why = nullptr;
};

Ahead &ahead()
{
    static Ahead *a = new Ahead;
    return *a;
}

#if SWITCH_METAL_DIAGNOSTICS
uint32_t encodersBefore(SwitchMetalCommandBuffer *cb, size_t position)
{
    uint32_t n = 0;
    for (size_t at = 0; at < position; ++at)
        n += SwitchMetalCommandPlan::isEncoderBegin(cb->_plan.operation(at).op);
    return n;
}
#endif

// Debug builds: `cb`'s waiters for the worker (switchMetalWaitAhead) woken, after what they read was stored
void aheadChanged(SwitchMetalCommandBuffer *cb)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    gm20bChanged(cb);
#else
    (void)cb;
#endif
}

// The encode ahead given back for `veto` (the first reason counted): the device's command buffer abandoned, and the
// command buffer encoded at its commit
void giveBack(uint32_t veto)
{
    Ahead &a = ahead();
    SwitchMetalCommandBuffer *cb = a.cb;
    if (!cb)
        return;
    if (a.open) {
        gm20bRangeEncode().reset();
        gRM->committed.fetch_sub(1);
        cb->_pipelineAbandons++;
        cb->_pipelineGivenBack |= veto;
        SWITCH_METAL_DIAGNOSTIC(gRM->stats.pipelineAbandons[__builtin_ctz(veto ? veto : uint32_t(VetoFailed))]++);
    }
    cb->_pipelineDeclined.store(true, std::memory_order_release);
    a.cb = nil;
    a.open = false;
    aheadChanged(cb);
}

}  // namespace

void gm20bPublishEncoder(SwitchMetalCommandBuffer *cb)
{
    const size_t length = cb->_plan.size();
    cb->_published.store(uint64_t(cb->_endedAt) << 32 | length, std::memory_order_seq_cst);
    cb->_endedAt = cb->_encAt;
    cb->_handedCommands += uint32_t(length - cb->_encAt);
    if (cb->_handedCommands < kHandOff || cb->_pipelineVeto.load(std::memory_order_relaxed))
        return;
    cb->_handedCommands = 0;
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.handoffs++);
    if (!cb->_handedOnce) {
        cb->_handedOnce = true;
        SWITCH_METAL_DIAGNOSTIC(gRM->stats.handoffWakes += gRM->submitAsleep.load(std::memory_order_relaxed));
        gRM->schedule.push({SwitchMetalScheduleChange::Encoded, cb});
        return;
    }
    // the worker holds it already and reads _published before it sleeps: a wake only for a worker asleep
    if (!cb->_pipelineDeclined.load(std::memory_order_relaxed) && gRM->submitAsleep.load(std::memory_order_seq_cst)) {
        SWITCH_METAL_DIAGNOSTIC(gRM->stats.handoffWakes++);
        gRM->schedule.wake();
    }
}

void gm20bPipelineHanded(SwitchMetalCommandBuffer *cb)
{
    Ahead &a = ahead();
    if (cb->_pipelineDeclined.load(std::memory_order_relaxed) || cb->_committed.load(std::memory_order_acquire))
        return;
    if (a.cb) {
        cb->_pipelineDeclined.store(true, std::memory_order_release);
        aheadChanged(cb);
        return;
    }
    a.cb = cb;
    a.open = false;
}

bool gm20bPipelinePublished()
{
    const Ahead &a = ahead();
    if (!a.cb)
        return false;
    const size_t until = size_t(a.cb->_published.load(std::memory_order_seq_cst) >> 32);
    return until > (a.open ? gm20bRangeEncode().position() : 0);
}

bool gm20bPipelineStep()
{
    Ahead &a = ahead();
    SwitchMetalCommandBuffer *cb = a.cb;
    if (!cb)
        return false;
    const uint64_t published = cb->_published.load(std::memory_order_acquire);
    if (const uint32_t veto = cb->_pipelineVeto.load(std::memory_order_acquire)) {
        giveBack(veto);
        return true;
    }
    const size_t until = size_t(published >> 32);
    SwitchMetalRangeEncode &encode = gm20bRangeEncode();
    if (a.open && encode.position() >= until)
        return false;
    if (!a.open && !until)
        return false;
    return gm20bSubmission([&] {
        if (!a.open) {
            // counted as committed, no purge takes an arena it encodes (gpu.h purgeArenas)
            gRM->committed.fetch_add(1);
            if (gRM->raw->purgedArenas()) {
                encode.reset();
                gRM->committed.fetch_sub(1);
                giveBack(VetoDevice);
                return true;
            }
            // an open() that stops (VetoChannel: the kernel gave it no channel) is given back as an encode that began,
            // its reason counted; the command buffer's own encode at its turn asks for the channel again
            a.open = true;
            if (!encode.open(cb, 0, SIZE_MAX, false, &a.why)) {
                giveBack(encode.vetoed());
                return true;
            }
        }
        if (!encode.step(until)) {
            giveBack(encode.vetoed());
            return true;
        }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        cb->_aheadAt.store(encode.position(), std::memory_order_release);
        aheadChanged(cb);
#endif
        return true;
    });
}

void gm20bPipelineYield(SwitchMetalCommandBuffer *cb, size_t from, bool nativeRange)
{
    Ahead &a = ahead();
    if (a.cb && !(a.cb == cb && nativeRange && from == 0))
        giveBack(VetoOtherWork);
}

void gm20bPipelineFailed(SwitchMetalCommandBuffer *cb)
{
    if (ahead().cb == cb)
        giveBack(cb->_pipelineVeto.load(std::memory_order_acquire) ?: uint32_t(VetoFailed));
}

bool gm20bPipelineTake(SwitchMetalCommandBuffer *cb, size_t from)
{
    Ahead &a = ahead();
    if (a.cb != cb || from)
        return false;
    if (!a.open) {
        giveBack(0);
        return false;
    }
    SwitchMetalRangeEncode &encode = gm20bRangeEncode();
    if (const uint32_t veto = cb->_pipelineVeto.load(std::memory_order_acquire)) {
        giveBack(veto);
        return false;
    }
    if (!gRM->raw->channelCurrent(encode.generation())) {
        giveBack(VetoDevice);
        return false;
    }
#if SWITCH_METAL_DIAGNOSTICS
    cb->_pipelinedEncoders = encodersBefore(cb, encode.position());
#endif
    gRM->committed.fetch_sub(1);
    a.cb = nil;
    a.open = false;
    return true;
}

void gm20bPipelineOrphans()
{
    Ahead &a = ahead();
    if (a.cb && !a.cb->_committed.load(std::memory_order_acquire) && CFGetRetainCount((__bridge CFTypeRef)a.cb) == 1)
        giveBack(VetoOtherWork);
}

void gm20bPipelineSealed(SwitchMetalCommandBuffer *cb, size_t to)
{
#if SWITCH_METAL_DIAGNOSTICS
    const uint32_t encoders = encodersBefore(cb, to);
    cb->_pipelineEncoders = encoders;
    gRM->stats.sealedEncoders += encoders;
    gRM->stats.pipelinedEncoders += cb->_pipelinedEncoders;
#else
    (void)cb;
    (void)to;
#endif
}
