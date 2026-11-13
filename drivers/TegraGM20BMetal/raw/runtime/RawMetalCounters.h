// SPDX-License-Identifier: MIT
// Counter sampling (raw_counters.h): the device's sample buffers and clock (RawMetalCounters.mm), and the samples a pass
// descriptor asks for on the tape and in the raw encode (RawMetalCounterEncode.mm).
#pragma once
#import "RawMetalInternal.h"
#include "raw_counters.h"

// AGXMTLCounterSampleBuffer's contract: sampleCount samples of 8 bytes, resolved from their records in write-combined memory
// of the buffer's own, zeroed at creation
@interface RMCounterSampleBuffer : RMObject <MTLCounterSampleBuffer> {
@public
    RMBuffer *_storage;
    NSUInteger _sampleCount;
}
@end

// mach_absolute_time in nanoseconds, the domain of every sample
uint64_t rmHostNanoseconds();

// The kernel's anchors of the GPU's clock to it (TegraGM20BClockABI.h): the table where this process maps it and its address
// in the GPU's; null and 0 where the kernel has none
struct RMClockAnchors {
    const volatile TegraGM20BAbiClockAnchors *table;
    uint64_t va;
};

const RMClockAnchors &rmClockAnchors();
// One reading of the table, all zero where there is none: m2v's replay of a resolve binds it as bytes
TegraGM20BAbiClockAnchors rmClockAnchorsNow();
// Whether this process made a counter sample buffer: before one, no pass descriptor can name one of this device's
bool rmCounterBuffersMade();
// The blit resolve's kernel (raw_counter_kernel.h), compiled by this device once; nil where it could not be
id<MTLComputePipelineState> rmCounterResolveState();

// A descriptor's sample buffer attachments as `e`'s samples to place (render passes: taken off `pass`, the command buffer's
// copy, so m2v's replay never meets this device's buffers); then those that start the encoder placed at `begin`, or those
// that end it at its OpEnd
void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLRenderPassDescriptor *pass);
void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLComputePassDescriptor *pass);
void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLBlitPassDescriptor *pass);
void rmPlaceCounterMarks(RMCommandBuffer *cb, RMEncoder *e, size_t at, bool starts);
// Samples of [from, to), which m2v replays: not taken (they resolve to 0, as never written)
void rmCounterMarksDropped(RMCommandBuffer *cb, size_t from, size_t to);

// The samples of a tape range as the raw encode reaches them: an encoder's starts on its own engine where it begins, its ends
// in a segment behind it once it ended
class RMCounterEmit {
public:
    RMCounterEmit(RMCommandBuffer *cb, size_t from, size_t to, raw::CommandBuffer &rc);
    bool at(size_t position) const;
    void started(size_t position, raw::RenderEncoder &e);
    void started(size_t position, raw::ComputeEncoder &e);
    void started(size_t position, raw::BlitEncoder &e);
    void ended(size_t position);

private:
    const RMCounterMark *begin(size_t position) const;
    const RMCounterMark *end(size_t position) const;
    uint64_t take(const RMCounterMark &m);

    raw::CommandBuffer &rc_;
    const RMCounterMark *first_ = nullptr, *last_ = nullptr;
};
