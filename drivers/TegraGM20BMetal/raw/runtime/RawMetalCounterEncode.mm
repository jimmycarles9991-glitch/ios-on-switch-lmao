// SPDX-License-Identifier: MIT
// Counter samples on the tape and in the raw encode: the stage boundaries a pass descriptor's sample buffer attachments name
// (the only sampling point supportsCounterSampling: answers YES for), and the blit encoder's resolve of samples on the GPU.
#import "RawMetalCounters.h"
#include <algorithm>
#include "raw_compute.h"
#include "raw_counter_kernel.h"

using CounterPoint = raw::counters::Point;

// The driver takes only its own buffer class (AGX::CounterSamplingContextGen1::setCounterSampleBuffer)
static RMCounterSampleBuffer *ownBuffer(id<MTLCounterSampleBuffer> b, const char *what)
{
    if (!b || [(id)b isKindOfClass:[RMCounterSampleBuffer class]])
        return (RMCounterSampleBuffer *)b;
    SWITCH_REFUSE("raw", "counter.buffer", "%s: a counter sample buffer of %s, not this device's: not sampled", what,
                  class_getName(object_getClass(b)));
    return nil;
}

static void pend(RMCommandBuffer *cb, RMEncoder *e, RMCounterSampleBuffer *b, NSUInteger index, CounterPoint point)
{
    switch (raw::counters::sampleIndex(index, b->_sampleCount)) {
    case raw::counters::Index::None:
        return;
    case raw::counters::Index::Invalid:
        SWITCH_REFUSE_DETAIL("raw", "counter.index", index, "Invalid sample index: %lu > %lu: not sampled",
                             (unsigned long)index, (unsigned long)b->_sampleCount);
        return;
    case raw::counters::Index::Sample:
        break;
    }
    [cb retain:b];
    e->_counterPending.push_back({0, point, uint32_t(index), b});
}

void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLRenderPassDescriptor *pass)
{
    if (!rmCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLRenderPassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (RMCounterSampleBuffer *b = ownBuffer(a.sampleBuffer, "a render pass")) {
            pend(cb, e, b, a.startOfVertexSampleIndex, CounterPoint::VertexStart);
            pend(cb, e, b, a.startOfFragmentSampleIndex, CounterPoint::FragmentStart);
            pend(cb, e, b, a.endOfVertexSampleIndex, CounterPoint::VertexEnd);
            pend(cb, e, b, a.endOfFragmentSampleIndex, CounterPoint::FragmentEnd);
        }
        a.sampleBuffer = nil;
    }
}

void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLComputePassDescriptor *pass)
{
    if (!rmCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLComputePassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (RMCounterSampleBuffer *b = ownBuffer(a.sampleBuffer, "a compute pass")) {
            pend(cb, e, b, a.startOfEncoderSampleIndex, CounterPoint::ComputeStart);
            pend(cb, e, b, a.endOfEncoderSampleIndex, CounterPoint::ComputeEnd);
        }
    }
}

void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLBlitPassDescriptor *pass)
{
    if (!rmCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLBlitPassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (RMCounterSampleBuffer *b = ownBuffer(a.sampleBuffer, "a blit pass")) {
            pend(cb, e, b, a.startOfEncoderSampleIndex, CounterPoint::BlitStart);
            pend(cb, e, b, a.endOfEncoderSampleIndex, CounterPoint::BlitEnd);
        }
    }
}

void rmPlaceCounterMarks(RMCommandBuffer *cb, RMEncoder *e, size_t at, bool starts)
{
    auto &pending = e->_counterPending;
    for (RMCounterMark &m : pending)
        if (raw::counters::isStart(m.point) == starts) {
            m.at = uint32_t(at);
            cb->_counterMarks.push_back(m);
        }
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [&](const RMCounterMark &m) {
                                     return raw::counters::isStart(m.point) == starts;
                                 }),
                  pending.end());
}

void rmCounterMarksDropped(RMCommandBuffer *cb, size_t from, size_t to)
{
    for (const RMCounterMark &m : cb->_counterMarks)
        if (m.at >= from && m.at < to) {
            SWITCH_REFUSE("raw", "counter.m2v", "an encoder with counter samples went to m2v: its samples read 0");
            return;
        }
}

static bool before(const RMCounterMark &m, size_t position)
{
    return m.at < position;
}

RMCounterEmit::RMCounterEmit(RMCommandBuffer *cb, size_t from, size_t to, raw::CommandBuffer &rc) : rc_(rc)
{
    const auto &marks = cb->_counterMarks;
    first_ = marks.data() + (std::lower_bound(marks.begin(), marks.end(), from, before) - marks.begin());
    last_ = marks.data() + (std::lower_bound(marks.begin(), marks.end(), to, before) - marks.begin());
}

const RMCounterMark *RMCounterEmit::begin(size_t position) const
{
    return std::lower_bound(first_, last_, position, before);
}

const RMCounterMark *RMCounterEmit::end(size_t position) const
{
    return std::lower_bound(first_, last_, position + 1, before);
}

bool RMCounterEmit::at(size_t position) const
{
    return begin(position) != end(position);
}

// The record's address, its buffer written by this submission
uint64_t RMCounterEmit::take(const RMCounterMark &m)
{
    RMBuffer *storage = m.buffer->_storage;
    useBuffer(&rc_, storage, true);
    storage->_gpuDirty.store(true, std::memory_order_release);
    return storage->_raw.va + uint64_t(m.index) * raw::counters::kRecordBytes;
}

void RMCounterEmit::started(size_t position, raw::RenderEncoder &e)
{
    for (const RMCounterMark *m = begin(position); m != end(position); m++)
        e.timestampAtStart(take(*m), m->point);
}

void RMCounterEmit::started(size_t position, raw::ComputeEncoder &e)
{
    for (const RMCounterMark *m = begin(position); m != end(position); m++)
        e.timestamp(take(*m));
}

void RMCounterEmit::started(size_t position, raw::BlitEncoder &e)
{
    for (const RMCounterMark *m = begin(position); m != end(position); m++)
        e.timestamp(take(*m), false);
}

void RMCounterEmit::ended(size_t position)
{
    raw::counters::Stamp stamps[8];
    size_t count = 0;
    for (const RMCounterMark *m = begin(position); m != end(position) && count < 8; m++)
        stamps[count++] = {take(*m), m->point};
    std::stable_sort(stamps, stamps + count, [](const raw::counters::Stamp &a, const raw::counters::Stamp &b) {
        return a.point < b.point;
    });
    rc_.timestamps(stamps, count);
}

id<MTLComputePipelineState> rmCounterResolveState()
{
    static id<MTLComputePipelineState> state;
    static std::once_flag once;
    std::call_once(once, [] {
        NSError *error = nil;
        id<MTLDevice> device = gRM->device;
        id<MTLLibrary> library = [device newLibraryWithSource:@(raw::counters::kResolveSource)
                                                      options:nil
                                                        error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@(raw::counters::kResolveFunction)];
        state = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!state)
            SWITCH_REFUSE("raw", "counter.resolve_state",
                          "the counter resolve kernel was not made (%s): no blit resolve",
                          error.localizedDescription.UTF8String ?: "no function");
    });
    return state;
}

@interface RMBlitEncoder (Counters)
@end
@implementation RMBlitEncoder (Counters)

// The driver's copy of the samples' words on the GPU, here a dispatch of the resolve kernel between the encoder's commands
// before and after it: the encoder is ended, the kernel's compute encoder recorded, and the encoder begun again
- (void)resolveCounters:(id<MTLCounterSampleBuffer>)sampleBuffer
                inRange:(NSRange)range
      destinationBuffer:(id<MTLBuffer>)destination
      destinationOffset:(NSUInteger)offset
{
    RMCounterSampleBuffer *b = ownBuffer(sampleBuffer, "resolveCounters");
    if (!b)
        return;
    if (!raw::counters::withinSamples(range.location, range.length, b->_sampleCount)) {
        SWITCH_REFUSE_DETAIL("raw", "counter.resolve", NSMaxRange(range),
                             "Cannot resolve MTLCounter data: out of bounds access (%lu of %lu samples)",
                             (unsigned long)NSMaxRange(range), (unsigned long)b->_sampleCount);
        return;
    }
    if (!range.length)
        return;
    RMBuffer *into = (RMBuffer *)destination;
    if (![(id)into isKindOfClass:[RMBuffer class]] || offset % 4 ||
        offset + range.length * raw::counters::kSampleBytes > into->_raw.length) {
        SWITCH_REFUSE_DETAIL("raw", "counter.resolve_into", offset,
                             "resolveCounters: into %lu bytes at %lu of a destination it does not fit, or off 4 bytes",
                             (unsigned long)(range.length * raw::counters::kSampleBytes), (unsigned long)offset);
        return;
    }
    id<MTLComputePipelineState> state = rmCounterResolveState();
    if (!state)
        return;
    const raw::counters::ResolveParams params = {uint32_t(range.length)};
    const RMClockAnchors &anchors = rmClockAnchors();
    RMCommandBuffer *cb = _cb;
    [cb record:cmd(OpEnd)];
    cb->_active = nil;
    id<MTLComputeCommandEncoder> c = [(id<MTLCommandBuffer>)cb computeCommandEncoder];
    [c setComputePipelineState:state];
    [c setBuffer:(id<MTLBuffer>)b->_storage offset:range.location * raw::counters::kRecordBytes atIndex:0];
    [c setBuffer:destination offset:offset atIndex:1];
    [c setBytes:&params length:sizeof params atIndex:2];
    // read where the GPU runs the resolve, so a power-up between this encode and the samples has its anchor
    if (anchors.va) {
        Cmd m = cmd(OpKernelClockAnchors, 3, 3);
        m.c = anchors.va;
        m.b = sizeof(TegraGM20BAbiClockAnchors);
        [cb record:m];
    } else {
        const TegraGM20BAbiClockAnchors none = {};
        [c setBytes:&none length:sizeof none atIndex:3];
    }
    const NSUInteger groups = (range.length + raw::counters::kResolveThreads - 1) / raw::counters::kResolveThreads;
    [c dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(raw::counters::kResolveThreads, 1, 1)];
    [c endEncoding];
    [cb begin:self];
    [cb record:cmd(OpBlit)];
}

@end

@interface RMEncoder (Counters)
@end
@implementation RMEncoder (Counters)

// The driver reports it unsupported (MTLReportFailure) on every encoder: supportsCounterSampling: answers NO for draw,
// dispatch and blit boundaries
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)index
                   withBarrier:(BOOL)barrier
{
    (void)sampleBuffer, (void)index, (void)barrier;
    SWITCH_REFUSE("raw", "call.counter_sample",
                  "-[%s sampleCountersInBuffer:atSampleIndex:withBarrier:] not supported on "
                  "this device",
                  class_getName(object_getClass(self)));
}

@end
