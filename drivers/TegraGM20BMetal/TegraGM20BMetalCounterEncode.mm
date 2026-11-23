// SPDX-License-Identifier: MIT
// Counter samples on the tape and in the raw encode: the stage boundaries a pass descriptor's sample buffer attachments name
// (the only sampling point supportsCounterSampling: answers YES for), and the blit encoder's resolve of samples on the GPU.
#import "TegraGM20BMetalCounters.h"
#include <algorithm>
#include "compute.h"
#include "counter_kernel.h"

using CounterPoint = gm20b::counters::Point;

// The driver takes only its own buffer class (AGX::CounterSamplingContextGen1::setCounterSampleBuffer): another
// device's fails the command buffer InvalidResource, as every object of another device does
static SwitchMetalCounterSampleBuffer *ownBuffer(SwitchMetalCommandBuffer *cb, id<MTLCounterSampleBuffer> b,
                                                 const char *what)
{
    if (!b || [(id)b isKindOfClass:[SwitchMetalCounterSampleBuffer class]])
        return (SwitchMetalCounterSampleBuffer *)b;
    gm20bCallInvalid(cb, PredCallCounter, 0, 0, what);
    return nil;
}

static void pend(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, SwitchMetalCounterSampleBuffer *b,
                 NSUInteger index, CounterPoint point)
{
    switch (gm20b::counters::sampleIndex(index, b->_sampleCount)) {
    case gm20b::counters::Index::None:
        return;
    case gm20b::counters::Index::Invalid:
        gm20bSkipped(PredCallCounter);
        SWITCH_REFUSE("gm20b", "counter.index", "Invalid sample index: %lu > %lu: not sampled", (unsigned long)index,
                      (unsigned long)b->_sampleCount);
        return;
    case gm20b::counters::Index::Sample:
        break;
    }
    [cb retain:b];
    gm20bPipelineVeto(cb, VetoCounters);
    e->_counterPending.push_back({0, point, uint32_t(index), b});
}

void gm20bCounterMarks(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, MTLRenderPassDescriptor *pass)
{
    if (!gm20bCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLRenderPassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (SwitchMetalCounterSampleBuffer *b =
                ownBuffer(cb, a.sampleBuffer, "a render pass's counter sample buffer not made by this device")) {
            pend(cb, e, b, a.startOfVertexSampleIndex, CounterPoint::VertexStart);
            pend(cb, e, b, a.startOfFragmentSampleIndex, CounterPoint::FragmentStart);
            pend(cb, e, b, a.endOfVertexSampleIndex, CounterPoint::VertexEnd);
            pend(cb, e, b, a.endOfFragmentSampleIndex, CounterPoint::FragmentEnd);
        }
        a.sampleBuffer = nil;
    }
}

void gm20bCounterMarks(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, MTLComputePassDescriptor *pass)
{
    if (!gm20bCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLComputePassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (SwitchMetalCounterSampleBuffer *b =
                ownBuffer(cb, a.sampleBuffer, "a compute pass's counter sample buffer not made by this device")) {
            pend(cb, e, b, a.startOfEncoderSampleIndex, CounterPoint::ComputeStart);
            pend(cb, e, b, a.endOfEncoderSampleIndex, CounterPoint::ComputeEnd);
        }
    }
}

void gm20bCounterMarks(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, MTLBlitPassDescriptor *pass)
{
    if (!gm20bCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLBlitPassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (SwitchMetalCounterSampleBuffer *b =
                ownBuffer(cb, a.sampleBuffer, "a blit pass's counter sample buffer not made by this device")) {
            pend(cb, e, b, a.startOfEncoderSampleIndex, CounterPoint::BlitStart);
            pend(cb, e, b, a.endOfEncoderSampleIndex, CounterPoint::BlitEnd);
        }
    }
}

void gm20bPlaceCounterMarks(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, size_t at, bool starts)
{
    auto &pending = e->_counterPending;
    for (SwitchMetalCounterMark &m : pending)
        if (gm20b::counters::isStart(m.point) == starts) {
            m.at = uint32_t(at);
            cb->_counterMarks.push_back(m);
        }
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [&](const SwitchMetalCounterMark &m) {
                                     return gm20b::counters::isStart(m.point) == starts;
                                 }),
                  pending.end());
}

static bool before(const SwitchMetalCounterMark &m, size_t position)
{
    return m.at < position;
}

SwitchMetalCounterEmit::SwitchMetalCounterEmit(SwitchMetalCommandBuffer *cb, size_t from, size_t to,
                                               gm20b::CommandBuffer &rc)
    : rc_(rc)
{
    const auto &marks = cb->_counterMarks;
    first_ = marks.data() + (std::lower_bound(marks.begin(), marks.end(), from, before) - marks.begin());
    last_ = marks.data() + (std::lower_bound(marks.begin(), marks.end(), to, before) - marks.begin());
}

const SwitchMetalCounterMark *SwitchMetalCounterEmit::begin(size_t position) const
{
    return std::lower_bound(first_, last_, position, before);
}

const SwitchMetalCounterMark *SwitchMetalCounterEmit::end(size_t position) const
{
    return std::lower_bound(first_, last_, position + 1, before);
}

bool SwitchMetalCounterEmit::at(size_t position) const
{
    return begin(position) != end(position);
}

// The record's address, its buffer written by this submission
uint64_t SwitchMetalCounterEmit::take(const SwitchMetalCounterMark &m)
{
    SwitchMetalBuffer *storage = m.buffer->_storage;
    useBuffer(&rc_, storage, true);
    storage->_gpuDirty.store(true, std::memory_order_release);
    return storage->_raw.va + uint64_t(m.index) * gm20b::counters::kRecordBytes;
}

void SwitchMetalCounterEmit::started(size_t position, gm20b::RenderEncoder &e)
{
    for (const SwitchMetalCounterMark *m = begin(position); m != end(position); m++)
        e.timestampAtStart(take(*m), m->point);
}

void SwitchMetalCounterEmit::started(size_t position, gm20b::ComputeEncoder &e)
{
    for (const SwitchMetalCounterMark *m = begin(position); m != end(position); m++)
        e.timestamp(take(*m));
}

void SwitchMetalCounterEmit::started(size_t position, gm20b::BlitEncoder &e)
{
    for (const SwitchMetalCounterMark *m = begin(position); m != end(position); m++)
        e.timestamp(take(*m), false);
}

void SwitchMetalCounterEmit::ended(size_t position)
{
    gm20b::counters::Stamp stamps[8];
    size_t count = 0;
    for (const SwitchMetalCounterMark *m = begin(position); m != end(position) && count < 8; m++)
        stamps[count++] = {take(*m), m->point};
    std::stable_sort(stamps, stamps + count, [](const gm20b::counters::Stamp &a, const gm20b::counters::Stamp &b) {
        return a.point < b.point;
    });
    rc_.timestamps(stamps, count);
}

static id<MTLComputePipelineState> resolveState(const char *name)
{
    NSError *error = nil;
    id<MTLDevice> device = gRM->device;
    id<MTLLibrary> library = [device newLibraryWithSource:@(gm20b::counters::kResolveSource) options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@(name)];
    id<MTLComputePipelineState> state =
        function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!state)
        SWITCH_REFUSE_DETAIL("gm20b", "counter.resolve_state", uintptr_t(name),
                             "the counter resolve kernel %s was not made (%s): no blit resolve of it", name,
                             error.localizedDescription.UTF8String ?: "no function");
    return state;
}

id<MTLComputePipelineState> gm20bCounterResolveState()
{
    static id<MTLComputePipelineState> state;
    static std::once_flag once;
    std::call_once(once, [] {
        state = resolveState(gm20b::counters::kResolveFunction);
    });
    return state;
}

// The resolve into a destination offset off 4 bytes, made at the first such resolve
static id<MTLComputePipelineState> resolveBytesState()
{
    static id<MTLComputePipelineState> state;
    static std::once_flag once;
    std::call_once(once, [] {
        state = resolveState(gm20b::counters::kResolveBytesFunction);
    });
    return state;
}

@interface SwitchMetalBlitEncoder (Counters)
@end
@implementation SwitchMetalBlitEncoder (Counters)

// The driver's copy of the samples' words on the GPU, here a dispatch of the resolve kernel between the encoder's commands
// before and after it: the encoder is ended, the kernel's compute encoder recorded, and the encoder begun again
- (void)resolveCounters:(id<MTLCounterSampleBuffer>)sampleBuffer
                inRange:(NSRange)range
      destinationBuffer:(id<MTLBuffer>)destination
      destinationOffset:(NSUInteger)offset
{
    SwitchMetalCounterSampleBuffer *b =
        ownBuffer(_cb, sampleBuffer, "resolveCounters: a counter sample buffer not made by this device");
    // an M4 Max resolves nothing of a nil one
    if (!sampleBuffer) {
        gm20bSkipped(PredCallCounter);
        SWITCH_REFUSE_DETAIL("gm20b", "counter.resolve", 1, "resolveCounters: a nil counter sample buffer: nothing");
    }
    if (!b)
        return;
    if (!gm20b::counters::withinSamples(range.location, range.length, b->_sampleCount)) {
        gm20bSkipped(PredCallCounter);
        SWITCH_REFUSE("gm20b", "counter.resolve",
                      "Cannot resolve MTLCounter data: out of bounds access (%lu of %lu samples)",
                      (unsigned long)NSMaxRange(range), (unsigned long)b->_sampleCount);
        return;
    }
    if (!range.length)
        return;
    SwitchMetalBuffer *into = (SwitchMetalBuffer *)destination;
    if (![(id)into isKindOfClass:[SwitchMetalBuffer class]])
        return gm20bCallInvalid(_cb, PredCallBufForeign, offset, 0,
                                destination ? "resolveCounters: a destination buffer not made by this device"
                                            : "resolveCounters: a nil destination buffer");
    // an M4 Max writes the samples at an offset off 4 bytes byte for byte from there
    const uint32_t shift = uint32_t(offset % 4);
    id<MTLComputePipelineState> state = shift ? resolveBytesState() : gm20bCounterResolveState();
    if (shift) {
        gm20bSkipped(PredCallCounter);
        SWITCH_REFUSE("gm20b", "counter.resolve_into", "resolveCounters: into offset %lu, off 4 bytes: %s",
                      (unsigned long)offset, state ? "resolved there" : "not resolved");
    }
    // an M4 Max resolves the samples the destination holds past the offset
    const uint64_t fits = offset < into->_raw.length ? (into->_raw.length - offset) / gm20b::counters::kSampleBytes : 0;
    if (range.length > fits) {
        gm20bSkipped(PredCallCounter);
        SWITCH_REFUSE_DETAIL("gm20b", "counter.resolve_into", 1,
                             "resolveCounters: %lu samples into %llu bytes at %lu: the %llu it holds resolved",
                             (unsigned long)range.length, (unsigned long long)into->_raw.length, (unsigned long)offset,
                             (unsigned long long)fits);
        range.length = NSUInteger(fits);
        if (!range.length)
            return;
    }
    if (!state)
        return;
    const gm20b::counters::ResolveParams params = {uint32_t(range.length)};
    const gm20b::counters::ResolveBytesParams shifted = {uint32_t(range.length), shift};
    const SwitchMetalClockAnchors &anchors = gm20bClockAnchors();
    SwitchMetalCommandBuffer *cb = _cb;
    [cb record:cmd(OpEnd)];
    cb->_active = nil;
    id<MTLComputeCommandEncoder> c = [(id<MTLCommandBuffer>)cb computeCommandEncoder];
    [c setComputePipelineState:state];
    [c setBuffer:(id<MTLBuffer>)b->_storage offset:range.location * gm20b::counters::kRecordBytes atIndex:0];
    [c setBuffer:destination offset:offset - shift atIndex:1];
    if (shift)
        [c setBytes:&shifted length:sizeof shifted atIndex:2];
    else
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
    const NSUInteger groups = (range.length + gm20b::counters::kResolveThreads - 1) / gm20b::counters::kResolveThreads;
    [c dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(gm20b::counters::kResolveThreads, 1, 1)];
    [c endEncoding];
    [cb begin:self];
    [cb record:cmd(OpBlit)];
}

@end

@interface SwitchMetalEncoder (Counters)
@end
@implementation SwitchMetalEncoder (Counters)

// The driver reports it unsupported (MTLReportFailure) on every encoder: supportsCounterSampling: answers NO for draw,
// dispatch and blit boundaries, and an M4 Max's driver aborts the process
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)index
                   withBarrier:(BOOL)barrier
{
    (void)sampleBuffer, (void)barrier;
    gm20bCallInvalid(
        _cb, PredCallCounter, index, 0,
        "sampleCountersInBuffer:atSampleIndex:withBarrier: on an encoder, a sampling point this device has "
        "none of");
}

@end
