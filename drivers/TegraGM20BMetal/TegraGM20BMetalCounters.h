// SPDX-License-Identifier: MIT
// Counter sampling (counters.h): the device's sample buffers and clock (TegraGM20BMetalCounters.mm), and the samples a pass
// descriptor asks for on the tape and in the raw encode (TegraGM20BMetalCounterEncode.mm).
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "counters.h"

// AGXMTLCounterSampleBuffer's contract: sampleCount samples of 8 bytes, resolved from their records in write-combined memory
// of the buffer's own, zeroed at creation
@interface SwitchMetalCounterSampleBuffer : SwitchMetalObject <MTLCounterSampleBuffer> {
@public
    SwitchMetalBuffer *_storage;
    NSUInteger _sampleCount;
}
@end

// mach_absolute_time in nanoseconds, the domain of every sample
uint64_t gm20bHostNanoseconds();

// The kernel's anchors of the GPU's clock to it (TegraGM20BClockABI.h): the table where this process maps it and its address
// in the GPU's; null and 0 where the kernel has none
struct SwitchMetalClockAnchors {
    const volatile TegraGM20BAbiClockAnchors *table;
    uint64_t va;
};

const SwitchMetalClockAnchors &gm20bClockAnchors();
// One reading of the table, all zero where there is none: m2v's replay of a resolve binds it as bytes
TegraGM20BAbiClockAnchors gm20bClockAnchorsNow();
// Whether this process made a counter sample buffer: before one, no pass descriptor can name one of this device's
bool gm20bCounterBuffersMade();
// The blit resolve's kernel (counter_kernel.h), compiled by this device once; nil where it could not be
id<MTLComputePipelineState> gm20bCounterResolveState();

// A descriptor's sample buffer attachments as `e`'s samples to place (render passes: taken off `pass`, the command buffer's
// copy, so m2v's replay never meets this device's buffers); then those that start the encoder placed at `begin`, or those
// that end it at its OpEnd
void gm20bCounterMarks(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, MTLRenderPassDescriptor *pass);
void gm20bCounterMarks(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, MTLComputePassDescriptor *pass);
void gm20bCounterMarks(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, MTLBlitPassDescriptor *pass);
void gm20bPlaceCounterMarks(SwitchMetalCommandBuffer *cb, SwitchMetalEncoder *e, size_t at, bool starts);

// The samples of a tape range as the raw encode reaches them: an encoder's starts on its own engine where it begins, its ends
// in a segment behind it once it ended
class SwitchMetalCounterEmit {
public:
    SwitchMetalCounterEmit(SwitchMetalCommandBuffer *cb, size_t from, size_t to, gm20b::CommandBuffer &rc);

    // None: an encode ahead's (a command buffer with samples is encoded at its commit)
    explicit SwitchMetalCounterEmit(gm20b::CommandBuffer &rc) : rc_(rc)
    {}

    bool at(size_t position) const;
    void started(size_t position, gm20b::RenderEncoder &e);
    void started(size_t position, gm20b::ComputeEncoder &e);
    void started(size_t position, gm20b::BlitEncoder &e);
    void ended(size_t position);

private:
    const SwitchMetalCounterMark *begin(size_t position) const;
    const SwitchMetalCounterMark *end(size_t position) const;
    uint64_t take(const SwitchMetalCounterMark &m);

    gm20b::CommandBuffer &rc_;
    const SwitchMetalCounterMark *first_ = nullptr, *last_ = nullptr;
};
