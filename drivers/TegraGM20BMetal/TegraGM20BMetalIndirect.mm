// SPDX-License-Identifier: MIT
// MTLIndirectCommandBuffer of draws that inherit their pipeline and buffers (icb.h): the commands are push words in
// write-combined GPU memory, written as the CPU encodes each, and a render encoder's executeCommandsInBuffer:withRange:
// runs the range by reference after one prepare of the state they inherit. Commands that set their own pipeline, buffers or
// raster state, and mesh draws, have no form here: such a descriptor's creation is nil. Patch commands are
// TegraGM20BMetalIndirectPatches.mm's. A descriptor of dispatches makes a buffer of them (TegraGM20BMetalIndirectCompute.mm).
#include "lock.h"
#import "TegraGM20BMetalIndirectCompute.h"
#include "icb.h"

namespace {

constexpr NSUInteger kDispatchTypes =
    MTLIndirectCommandTypeConcurrentDispatch | MTLIndirectCommandTypeConcurrentDispatchThreads;
constexpr NSUInteger kPatchTypes = MTLIndirectCommandTypeDrawPatches | MTLIndirectCommandTypeDrawIndexedPatches;

const char *unsupportedDescriptor(MTLIndirectCommandBufferDescriptor *d)
{
    if (d.commandTypes &
        ~NSUInteger(MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed | kDispatchTypes | kPatchTypes))
        return "mesh draws";
    if (d.commandTypes & kDispatchTypes)
        return nullptr;
    if (!d.inheritPipelineState || !d.inheritBuffers)
        return "commands that set their own pipeline or buffers";
    for (NSString *inherits : @[
             @"inheritDepthStencilState", @"inheritDepthBias", @"inheritCullMode", @"inheritDepthClipMode",
             @"inheritFrontFacingWinding", @"inheritTriangleFillMode"
         ])
        if ([d respondsToSelector:NSSelectorFromString(inherits)] && ![[d valueForKey:inherits] boolValue])
            return "commands that set their own raster or depth/stencil state";
    return nullptr;
}

// The SwitchMetalICBKinds bits of command `c` (counted in SwitchMetalIndirectCommandBuffer::_kinds by bit index)
uint8_t kindsOf(const SwitchMetalICBCommand &c)
{
    if (c.kind == SwitchMetalICBCommand::None)
        return 0;
    if (c.kind == SwitchMetalICBCommand::Patches)
        return SwitchMetalICBPatches;
    return uint8_t((gm20b::icb::triangles(c.primitive) ? SwitchMetalICBTriangles : SwitchMetalICBOthers) |
                   (c.kind == SwitchMetalICBCommand::Indexed ? SwitchMetalICBIndexed : 0));
}

}  // namespace

@implementation SwitchMetalIndirectCommandBuffer

- (void)dealloc
{
    if (_raw.va)
        gRM->raw->destroyBuffer(_raw);
}

// The buffers and kinds of the whole buffer follow each command's
- (void)take:(SwitchMetalICBCommand)command at:(NSUInteger)index
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    SwitchMetalICBCommand &was = _commands[index];
    if (command.kind != SwitchMetalICBCommand::None && !command.invalid) {
        command.invalid = was.invalid;
        command.invalidWhy = was.invalidWhy;
    }
    if (bool(was.invalid) != bool(command.invalid))
        _invalid.fetch_add(command.invalid ? 1 : uint32_t(-1), std::memory_order_relaxed);
    for (SwitchMetalBuffer *read : {was.indices, was.factors, was.patchIndices, was.pointIndices})
        if (read && --_buffers[(__bridge void *)read] == 0)
            _buffers.erase((__bridge void *)read);
    for (SwitchMetalBuffer *read : {command.indices, command.factors, command.patchIndices, command.pointIndices})
        if (read)
            _buffers[(__bridge void *)read]++;
    for (uint32_t bit = 0; bit < 4; bit++) {
        _kinds[bit] -= kindsOf(was) >> bit & 1;
        _kinds[bit] += kindsOf(command) >> bit & 1;
    }
    was = command;
}

- (uint8_t)kinds
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    uint8_t kinds = 0;
    for (uint32_t bit = 0; bit < 4; bit++)
        kinds |= uint8_t((_kinds[bit] ? 1 : 0) << bit);
    return kinds;
}

- (SwitchMetalPred)invalidIn:(NSUInteger)first count:(NSUInteger)count why:(const char **)why
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    for (NSUInteger i = first; i < std::min<NSUInteger>(first + count, _count); i++)
        if (_commands[i].invalid) {
            *why = _commands[i].invalidWhy;
            return _commands[i].invalid;
        }
    return PredNone;
}

- (std::vector<SwitchMetalBuffer *>)buffers
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    std::vector<SwitchMetalBuffer *> out;
    for (const auto &[buffer, uses] : _buffers)
        out.push_back((__bridge SwitchMetalBuffer *)buffer);
    return out;
}

- (NSUInteger)size
{
    return _count;
}

- (id<MTLIndirectRenderCommand>)indirectRenderCommandAtIndex:(NSUInteger)index
{
    if (index >= _count)
        return nil;
    SwitchMetalIndirectRenderCommand *c = [SwitchMetalIndirectRenderCommand new];
    c->_buffer = self;
    c->_index = index;
    return (id<MTLIndirectRenderCommand>)c;
}

- (id<MTLIndirectComputeCommand>)indirectComputeCommandAtIndex:(NSUInteger)index
{
    (void)index;
    return nil;
}

- (void)resetWithRange:(NSRange)range
{
    for (NSUInteger i = range.location; i < std::min<NSUInteger>(NSMaxRange(range), _count); i++) {
        gm20b::icb::empty(gm20bCommandSlot(self, i), _slotWords);
        [self take:SwitchMetalICBCommand {} at:i];
    }
}

- (MTLResourceID)gpuResourceID
{
    MTLResourceID id;
    static_assert(sizeof id == sizeof _raw.va);
    memcpy(&id, &_raw.va, sizeof id);
    return id;
}

- (NSUInteger)allocatedSize
{
    return NSUInteger(_raw.length);
}

- (MTLStorageMode)storageMode
{
    return MTLStorageModeShared;
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return MTLCPUCacheModeWriteCombined;
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return MTLHazardTrackingModeTracked;
}

- (MTLResourceOptions)resourceOptions
{
    return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    (void)state;
    return MTLPurgeableStateNonVolatile;
}

@end

@implementation SwitchMetalIndirectRenderCommand

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)baseInstance
{
    gm20b::icb::draw(gm20bCommandSlot(_buffer, _index), uint32_t(_index),
                     {uint32_t(type), uint32_t(count), uint32_t(instances), uint32_t(start), uint32_t(baseInstance)},
                     _buffer->_slotWords);
    SwitchMetalICBCommand c;
    c.kind = SwitchMetalICBCommand::Draw;
    c.primitive = uint8_t(type);
    c.first = uint32_t(start);
    c.count = uint32_t(count);
    c.instances = uint32_t(instances);
    c.baseInstance = uint32_t(baseInstance);
    [_buffer take:c at:_index];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance
{
    SwitchMetalBuffer *b = [(id)buffer isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)buffer : nil;
    const uint32_t size = indexType == MTLIndexTypeUInt16 ? 2 : 4;
    if (!b)
        return [self invalidate:PredCallBufForeign
                            why:buffer ? "an indirect command's index buffer not made by this device"
                                       : "an indirect command's nil index buffer"];
    const NSUInteger read = offset - offset % size;
    if (!b->_raw.va || read >= b->_raw.length) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE("gm20b", "icb.indices", "indirect command %lu: an index buffer offset %lu past it",
                      (unsigned long)_index, (unsigned long)offset);
        [self reset];
        return;
    }
    // an M4 Max reads the indices of an offset off their type from the offset rounded down to it
    if (read != offset) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.indices", 1,
                             "indirect command %lu: an index buffer offset %lu off its type: rounded down to it",
                             (unsigned long)_index, (unsigned long)offset);
        offset = read;
    }
    gm20b::icb::indexed(gm20bCommandSlot(_buffer, _index), uint32_t(_index),
                        {uint32_t(type), size, b->_raw.va + offset, b->_raw.va + b->_raw.length - 1, uint32_t(count),
                         uint32_t(instances), 0, int32_t(baseVertex), uint32_t(baseInstance)},
                        _buffer->_slotWords);
    SwitchMetalICBCommand c;
    c.kind = SwitchMetalICBCommand::Indexed;
    c.primitive = uint8_t(type);
    c.count = uint32_t(count);
    c.instances = uint32_t(instances);
    c.baseVertex = int32_t(baseVertex);
    c.baseInstance = uint32_t(baseInstance);
    c.indexType = uint8_t(indexType);
    c.indexOffset = offset;
    c.indices = b;
    [_buffer take:c at:_index];
}

- (void)reset
{
    gm20b::icb::empty(gm20bCommandSlot(_buffer, _index), _buffer->_slotWords);
    [_buffer take:SwitchMetalICBCommand {} at:_index];
}

- (void)invalidate:(SwitchMetalPred)p why:(const char *)why
{
    gm20b::icb::empty(gm20bCommandSlot(_buffer, _index), _buffer->_slotWords);
    SwitchMetalICBCommand c;
    c.invalid = p;
    c.invalidWhy = why;
    [_buffer take:c at:_index];
}

// The buffer inherits these from the encoder (its descriptor's creation said so): Metal's validation refuses them. An
// M4 Max's driver faults setting a pipeline, and takes a buffer
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
    (void)state;
    [self invalidate:PredCallIcb why:"an indirect command set a pipeline its buffer inherits"];
}

- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)buffer, (void)offset, (void)index;
    gm20bSkipped(PredCallIcb);
    SWITCH_REFUSE_DETAIL("gm20b", "icb.inherited", 1, "an indirect command set a buffer its buffer inherits: ignored");
}

- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)buffer, (void)offset, (void)index;
    gm20bSkipped(PredCallIcb);
    SWITCH_REFUSE_DETAIL("gm20b", "icb.inherited", 1, "an indirect command set a buffer its buffer inherits: ignored");
}

@end

@interface SwitchMetalDevice (Indirect)
@end
@implementation SwitchMetalDevice (Indirect)

- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)d
                                                       maxCommandCount:(NSUInteger)count
                                                               options:(MTLResourceOptions)options
{
    (void)options;
    if (const char *why = unsupportedDescriptor(d)) {
        SWITCH_REFUSE_DETAIL("gm20b", "icb.descriptor", uintptr_t(why), "indirect command buffer of %s: nil", why);
        return nil;
    }
    if (d.commandTypes & kDispatchTypes)
        return gm20bNewComputeCommandBuffer(self, d, count);
    SwitchMetalIndirectCommandBuffer *b = [SwitchMetalIndirectCommandBuffer new];
    b->_device = self;
    b->_count = count;
    b->_patches = (d.commandTypes & kPatchTypes) != 0;
    b->_slotWords = b->_patches ? gm20b::icb::kPatchSlotWords : gm20b::icb::kSlotWords;
    b->_commands.resize(count);
    if (!count || !gRM->raw->createBuffer(uint64_t(count) * b->_slotWords * 4, b->_raw, true)) {
        SWITCH_REFUSE("gm20b", "icb.memory", "no memory for an indirect command buffer of %lu commands",
                      (unsigned long)count);
        return nil;
    }
    for (NSUInteger i = 0; i < count; i++)
        gm20b::icb::empty(gm20bCommandSlot(b, i), b->_slotWords);
    return (id<MTLIndirectCommandBuffer>)b;
}

@end

@implementation SwitchMetalRenderEncoder (Indirect)

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    [self execute:buffer first:range.location count:range.length range:nil offset:0];
}

// The range the GPU reads at `offset` of `rangeBuffer` (an MTLIndirectCommandBufferExecutionRange) selects the commands:
// every command is referenced, and those outside it draw nothing (mme.h kIcbDraw)
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                 indirectBuffer:(id<MTLBuffer>)rangeBuffer
           indirectBufferOffset:(NSUInteger)offset
{
    SwitchMetalBuffer *r =
        [(id)rangeBuffer isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)rangeBuffer : nil;
    if (!r)
        return gm20bCallInvalid(_cb, PredCallBufForeign, offset, 0,
                                rangeBuffer ? "an execute's range buffer not made by this device"
                                            : "an execute's nil range buffer");
    // an M4 Max's GPU reads the range at the offset rounded down to 4 bytes, and past its buffer reads whatever lies
    // there: nothing drawn, commands drawn or a page fault, by those bytes
    const NSUInteger read = offset & ~NSUInteger(3);
    if (read > r->_raw.length || r->_raw.length - read < 8) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 8,
                             "executeCommandsInBuffer:indirectBuffer: a range read past its buffer: none run");
        return;
    }
    if (read != offset) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 9,
                             "executeCommandsInBuffer:indirectBuffer: a range offset off 4 bytes: read from it "
                             "rounded down");
    }
    SwitchMetalIndirectCommandBuffer *b = [(id)buffer isKindOfClass:[SwitchMetalIndirectCommandBuffer class]]
                                              ? (SwitchMetalIndirectCommandBuffer *)buffer
                                              : nil;
    [self execute:buffer first:0 count:b ? b->_count : 0 range:r offset:read];
}

- (void)execute:(id<MTLIndirectCommandBuffer>)buffer
          first:(NSUInteger)first
          count:(NSUInteger)count
          range:(SwitchMetalBuffer *)rangeBuffer
         offset:(NSUInteger)offset
{
    SWITCH_API(ApiDraw);
    SwitchMetalIndirectCommandBuffer *b = [(id)buffer isKindOfClass:[SwitchMetalIndirectCommandBuffer class]]
                                              ? (SwitchMetalIndirectCommandBuffer *)buffer
                                              : nil;
    if (!b)
        return gm20bCallInvalid(_cb, PredCallIcb, first, count,
                                buffer ? "an execute of an indirect command buffer not made by this device"
                                       : "an execute of a nil indirect command buffer");
    if ([(id)b isKindOfClass:[SwitchMetalIndirectComputeBuffer class]]) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 1,
                             "executeCommandsInBuffer: a buffer of dispatches on a render "
                             "encoder: none run");
        return;
    }
    // an M4 Max runs the commands of the range inside the buffer
    if (first + count > b->_count) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 2, "executeCommandsInBuffer: a range past its buffer: cut to it");
        count = first < b->_count ? b->_count - first : 0;
    }
    if (!count)
        return;
    if (b->_invalid.load(std::memory_order_relaxed)) {
        const char *why = nullptr;
        if (const SwitchMetalPred p = [b invalidIn:first count:count why:&why])
            return gm20bCallInvalid(_cb, p, first, count, why);
    }
    const uint8_t kinds = [b kinds];
    const std::vector<SwitchMetalBuffer *> reads = [b buffers];
    if (![self checkDraw])
        return;
    [_cb retain:b];
    [_cb retain:rangeBuffer];
    for (SwitchMetalBuffer *read : reads)
        [_cb retain:read];
    // each command sets its own topology and polygon offset (gm20b::icb::polygonOffset), so triangles and lines or points
    // share an execute; a buffer of patches and primitives draws those of the inherited state's kind, the others under it
    // being Metal's validation error
    const bool patchState = _pipe && _pipe->_raw && _pipe->_raw->rec && gm20b::tessellates(*_pipe->_raw->rec);
    const bool patches =
        (kinds & SwitchMetalICBPatches) && (patchState || !(kinds & (SwitchMetalICBTriangles | SwitchMetalICBOthers)));
    const uint8_t primitive = patches ? uint8_t(gm20b::icb::kPatchPrimitive) : kinds & SwitchMetalICBTriangles ? 3 : 0;
    Cmd m = cmd(OpExecuteCommands, primitive);
    m.obj = (__bridge void *)rangeBuffer;
    m.obj2 = (__bridge void *)b;
    m.a = (kinds & SwitchMetalICBIndexed) != 0;
    m.b = uint32_t(offset);
    m.x = first;
    m.c = count;
    [_cb record:m];
}

@end

bool encodeCommands(gm20b::RenderEncoder *re, gm20b::CommandBuffer *rc, const SwitchMetalPlanDraw &draw,
                    const char **why)
{
    SwitchMetalIndirectCommandBuffer *b = (__bridge SwitchMetalIndirectCommandBuffer *)draw.arguments;
    std::vector<const gm20b::Buffer *> reads;
    for (SwitchMetalBuffer *buffer : [b buffers]) {
        useBuffer(rc, buffer, false);
        reads.push_back(&buffer->_raw);
    }
    SwitchMetalBuffer *range = (__bridge SwitchMetalBuffer *)draw.indices;
    if (range)
        useBuffer(rc, range, false);
    return re->executeSlots({&b->_raw, uint32_t(draw.argumentOffset), uint32_t(draw.indexOffset), b->_slotWords,
                             range ? &range->_raw : nullptr, draw.first, draw.primitive, draw.indexType != 0,
                             b->_gpuWritten.load(std::memory_order_relaxed), reads.data(), reads.size()},
                            why);
}

// Every draw a command of a buffer makes takes the encoder's pipeline (the buffer inherits it), so any render state draws
// one
@implementation SwitchMetalPipeline (Indirect)

- (BOOL)supportIndirectCommandBuffers
{
    return YES;
}

@end
