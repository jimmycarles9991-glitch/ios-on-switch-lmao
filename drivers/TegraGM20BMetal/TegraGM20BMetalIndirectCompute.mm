// SPDX-License-Identifier: MIT
// MTLIndirectCommandBuffer of dispatches (icb_compute.h): a command's QMDs, root tables and set 0 are written into
// the buffer's own GPU memory as the CPU encodes it, with its own pipeline's kernels made then, and its slot's words
// launch them. A buffer that inherits the encoder's buffers also writes each command's bind slot, from which an
// execute's GPU writes its descriptors; one that inherits the encoder's pipeline keeps its commands as the CPU encoded
// them and writes them all for a pipeline at the first execute under it after a change (gm20bPipelineVariant). A command
// that launches or binds more than its buffer's slots hold grows them all, written again from what the CPU keeps of
// each command.
#include "lock.h"
#import "TegraGM20BMetalIndirectCompute.h"

@interface SwitchMetalIndirectComputeBuffer ()
- (void)write:(NSUInteger)index;
@end

@implementation SwitchMetalIndirectComputeBuffer

- (void)dealloc
{
    for (gm20b::Buffer &chunk : _chunks)
        gRM->raw->destroyBuffer(chunk);
    for (gm20b::Buffer &old : _retired)
        if (old.va)
            gRM->raw->destroyBuffer(old);
    if (_binds.va)
        gRM->raw->destroyBuffer(_binds);
}

// Command `index`'s words, from what the CPU encoded: its regions written now where it inherits nothing
- (void)write:(NSUInteger)index
{
    SwitchMetalICBDispatch &c = _dispatches[index];
    c.raw = false;
    c.binds.clear();
    c.strideWords.clear();
    c.roots.clear();
    c.bindSlots = c.strideSlots = 0;
    _bindsChanged = true;
    _generation++;
    gm20b::ComputeDispatch shape;
    c.unmade = {};
    if (!_inheritPipeline)
        c.kernels = gm20bCommandKernels(c.pipeline, c, shape, c.unmade);
    if (_inheritPipeline || !c.kernels) {
        if (c.unmade.why)
            SWITCH_REFUSE_DETAIL("gm20b", "icb.kernel", uintptr_t(c.unmade.why), "indirect dispatch command %lu: %s",
                                 (unsigned long)index, c.unmade.why);
        gm20bWriteWords(self, index);
        return;
    }
    const uint64_t bytes = gm20bCommandBytes(*c.kernels, shape);
    if (bytes > UINT32_MAX)
        c.block = nullptr;
    else if (!c.block || c.block.use_count() > 1 || c.block->bytes < bytes)
        c.block = gm20bNewBlock(self, uint32_t(bytes));
    gm20b::ComputeEncoder::IcbBuffer buffers[31];
    if (!_inheritBuffers)
        gm20bCommandBuffers(c, buffers);
    const char *why = nullptr;
    if (!c.block || !gm20bCommandRegions(*c.kernels, shape, c, buffers, c.strides, *c.block, c.words, &why) ||
        (_inheritBuffers && !gm20bCommandBinds(*c.kernels, shape, *c.block, c, &why))) {
        c.unmade = c.block ? SwitchMetalICBUnmade{why}
                           : SwitchMetalICBUnmade{"no memory for its regions", gm20b::ComputeFailure::OutOfMemory};
        SWITCH_REFUSE_DETAIL("gm20b", "icb.region", uintptr_t(c.unmade.why), "indirect dispatch command %lu: %s",
                             (unsigned long)index, c.unmade.why);
        gm20bWriteWords(self, index);
        return;
    }
    c.words.barrier = c.barrier;
    c.raw = true;
    if (!gm20bFitSlots(self, gm20bSlotWords(c), gm20bBindWords(c))) {
        c.raw = false;
        c.unmade = {"no memory for its buffer's slots of its launches or binds", gm20b::ComputeFailure::OutOfMemory};
        SWITCH_REFUSE_DETAIL("gm20b", "icb.region", uintptr_t(c.unmade.why), "indirect dispatch command %lu: %s",
                             (unsigned long)index, c.unmade.why);
    }
    gm20bWriteWords(self, index);
}

- (id<MTLIndirectRenderCommand>)indirectRenderCommandAtIndex:(NSUInteger)index
{
    (void)index;
    return nil;
}

- (id<MTLIndirectComputeCommand>)indirectComputeCommandAtIndex:(NSUInteger)index
{
    if (index >= _count)
        return nil;
    SwitchMetalIndirectComputeCommand *c = [SwitchMetalIndirectComputeCommand new];
    c->_buffer = self;
    c->_index = index;
    return (id<MTLIndirectComputeCommand>)c;
}

- (void)resetWithRange:(NSRange)range
{
    std::lock_guard<gm20b::Mutex> hold(_lock);
    for (NSUInteger i = range.location; i < std::min<NSUInteger>(NSMaxRange(range), _count); i++) {
        _dispatches[i] = {};
        gm20bWriteWords(self, i);
    }
    _bindsChanged = true;
    _generation++;
}

@end

@implementation SwitchMetalIndirectComputeCommand

- (void)edit:(void (^)(SwitchMetalICBDispatch &c))change
{
    std::lock_guard<gm20b::Mutex> hold(_buffer->_lock);
    change(_buffer->_dispatches[_index]);
    [_buffer write:_index];
}

// A barrier changes the slot's first words alone
- (void)barrier:(bool)barrier
{
    std::lock_guard<gm20b::Mutex> hold(_buffer->_lock);
    SwitchMetalICBDispatch &c = _buffer->_dispatches[_index];
    c.barrier = barrier;
    if (!c.raw)
        return;
    c.words.barrier = barrier;
    gm20b::icb::dispatch(gm20bComputeSlot(_buffer, _index), uint32_t(_index), c.words, _buffer->_slotWords);
}

// Marked invalid with `p` and `why` (SwitchMetalICBDispatch::invalid)
- (void)invalidate:(SwitchMetalPred)p why:(const char *)why
{
    [self edit:^(SwitchMetalICBDispatch &c) {
        c.invalid = p;
        c.invalidWhy = why;
    }];
}

// What the buffer inherits is Metal's validation error: an M4 Max's driver faults setting a pipeline, and takes a
// buffer
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
    if (_buffer->_inheritPipeline)
        return [self invalidate:PredCallIcb why:"an indirect dispatch command set a pipeline its buffer inherits"];
    SwitchMetalComputeState *s =
        [(id)state isKindOfClass:[SwitchMetalComputeState class]] ? (SwitchMetalComputeState *)state : nil;
    [self edit:^(SwitchMetalICBDispatch &c) {
        c.pipeline = s;
    }];
}

- (void)setKernelBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self setKernelBuffer:buffer offset:offset attributeStride:NSUIntegerMax atIndex:index];
}

// A stage-in kernel's layout of MTLBufferLayoutStrideDynamic reads the stride (StageInStride); any other ignores it
- (void)setKernelBuffer:(id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
        attributeStride:(NSUInteger)stride
                atIndex:(NSUInteger)index
{
    if (_buffer->_inheritBuffers || index >= 31) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE("gm20b", "icb.inherited",
                      "an indirect dispatch command set a buffer its buffer inherits or past 30: ignored");
        return;
    }
    SwitchMetalBuffer *b = [(id)buffer isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)buffer : nil;
    if (buffer && !b)
        return [self invalidate:PredCompute why:"an indirect dispatch command's buffer not made by this device"];
    [self edit:^(SwitchMetalICBDispatch &c) {
        c.buffers[index] = b;
        c.offsets[index] = offset;
        c.strides[index] = stride >= gm20b::kNoBindStride ? gm20b::kNoBindStride : uint32_t(stride);
    }];
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
    const uint32_t bytes = uint32_t((length + 15) & ~NSUInteger(15));
    [self edit:^(SwitchMetalICBDispatch &c) {
        gm20b::ComputeSpecialization s;
        s.memory = c.memory;
        s.bindMemory(uint32_t(index), bytes);
        c.memory = s.memory;
    }];
}

- (void)setStageInRegion:(MTLRegion)region
{
    [self edit:^(SwitchMetalICBDispatch &c) {
        c.stageIn = true;
        c.stageInRegion = region;
    }];
}

- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height
{
    (void)width, (void)height;
    gm20bSkipped(PredCallIcb);
    SWITCH_REFUSE("gm20b", "icb.imageblock",
                  "setImageblockWidth:height: on an indirect dispatch command: no imageblocks");
}

- (void)concurrentDispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads
{
    [self dispatch:groups threads:threads perThread:false];
}

- (void)concurrentDispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)threads
{
    [self dispatch:grid threads:threads perThread:true];
}

- (void)dispatch:(MTLSize)grid threads:(MTLSize)threads perThread:(bool)perThread
{
    [self edit:^(SwitchMetalICBDispatch &c) {
        c.dispatched = true;
        c.perThread = perThread;
        c.grid = {grid.width, grid.height, grid.depth};
        c.threads = {uint32_t(threads.width), uint32_t(threads.height), uint32_t(threads.depth)};
    }];
}

- (void)setBarrier
{
    [self barrier:true];
}

- (void)clearBarrier
{
    [self barrier:false];
}

- (void)reset
{
    [self edit:^(SwitchMetalICBDispatch &c) {
        c = {};
    }];
}

@end

@implementation SwitchMetalKernelExecute
@end

id gm20bNewComputeCommandBuffer(id device, MTLIndirectCommandBufferDescriptor *d, NSUInteger count)
{
    SwitchMetalIndirectComputeBuffer *b = [SwitchMetalIndirectComputeBuffer new];
    b->_device = device;
    b->_count = count;
    b->_descriptor = [d copy];
    b->_inheritPipeline = d.inheritPipelineState;
    b->_inheritBuffers = d.inheritBuffers;
    b->_dispatches.resize(count);
    b->_slotWords = gm20b::icb::kComputeSlotWords;
    b->_bindWords = gm20b::icb::kComputeBindSlotWords;
    if (!count || !gRM->raw->createBuffer(uint64_t(count) * b->_slotWords * 4, b->_raw, true)) {
        SWITCH_REFUSE("gm20b", "icb.memory", "no memory for an indirect command buffer of %lu dispatches",
                      (unsigned long)count);
        return nil;
    }
    if (b->_inheritBuffers && !gRM->raw->createBuffer(uint64_t(count) * b->_bindWords * 4, b->_binds, true)) {
        SWITCH_REFUSE("gm20b", "icb.memory",
                      "no memory for the bind slots of an indirect command buffer of %lu dispatches",
                      (unsigned long)count);
        return nil;
    }
    for (NSUInteger i = 0; i < count; i++)
        gm20bWriteWords(b, i);
    return (id<MTLIndirectCommandBuffer>)b;
}

void gm20bWriteCommand(SwitchMetalIndirectComputeBuffer *b, NSUInteger index)
{
    [b write:index];
}
