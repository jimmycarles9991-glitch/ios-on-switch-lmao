// SPDX-License-Identifier: MIT
// MTLIndirectCommandBuffer of draws that inherit their pipeline and buffers (raw_icb.h): the commands are push words in
// write-combined GPU memory, written as the CPU encodes each, and a render encoder's executeCommandsInBuffer:withRange:
// runs the range by reference after one prepare of the state they inherit. Commands that set their own pipeline, buffers or
// raster state, and mesh draws, have no form here: such a descriptor's creation is nil. Patch commands
// (RawMetalIndirectPatches.mm) where the device answers supportsIndirectTessellation. A descriptor of dispatches makes a
// buffer of them (RawMetalIndirectCompute.mm).
#include "raw_lock.h"
#import "RawMetalIndirectCompute.h"
#include "raw_device_answers.h"
#include "raw_icb.h"

namespace {

constexpr NSUInteger kDispatchTypes =
    MTLIndirectCommandTypeConcurrentDispatch | MTLIndirectCommandTypeConcurrentDispatchThreads;
constexpr NSUInteger kPatchTypes = MTLIndirectCommandTypeDrawPatches | MTLIndirectCommandTypeDrawIndexedPatches;

bool indirectTessellation()
{
    return raw::answers::thisKindCount("supportsIndirectTessellation") != 0;
}

const char *unsupportedDescriptor(MTLIndirectCommandBufferDescriptor *d)
{
    const NSUInteger patches = indirectTessellation() ? kPatchTypes : 0;
    if (d.commandTypes &
        ~NSUInteger(MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed | kDispatchTypes | patches))
        return "patches or mesh draws";
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

// The RMICBKinds bits of command `c` (counted in RMIndirectCommandBuffer::_kinds by bit index)
uint8_t kindsOf(const RMICBCommand &c)
{
    if (c.kind == RMICBCommand::None)
        return 0;
    if (c.kind == RMICBCommand::Patches)
        return RMICBPatches;
    return uint8_t((raw::icb::triangles(c.primitive) ? RMICBTriangles : RMICBOthers) |
                   (c.kind == RMICBCommand::Indexed ? RMICBIndexed : 0));
}

}  // namespace

void rmWriteCommand(RMIndirectCommandBuffer *b, NSUInteger index, const RMICBCommand &c)
{
    uint32_t *slot = rmCommandSlot(b, index);
    if (c.kind == RMICBCommand::Draw)
        raw::icb::draw(slot, uint32_t(index), {c.primitive, c.count, c.instances, c.first, c.baseInstance},
                       b->_slotWords);
    else if (c.kind == RMICBCommand::Indexed)
        raw::icb::indexed(slot, uint32_t(index),
                          {c.primitive, c.indexType == MTLIndexTypeUInt16 ? 2u : 4u, c.indices->_raw.va + c.indexOffset,
                           c.indices->_raw.va + c.indices->_raw.length - 1, c.count, c.instances, 0, c.baseVertex,
                           c.baseInstance},
                          b->_slotWords);
    else if (c.kind == RMICBCommand::Patches && b->_slotWords >= raw::icb::kPatchSlotWords)
        raw::icb::patches(slot, uint32_t(index), c.patches);
    else
        raw::icb::empty(slot, b->_slotWords);
}

@implementation RMIndirectCommandBuffer

- (void)dealloc
{
    if (_raw.va)
        gRM->raw->destroyBuffer(_raw);
}

// The buffers and kinds of the whole buffer follow each command's
- (void)take:(RMICBCommand)command at:(NSUInteger)index
{
    std::lock_guard<raw::Mutex> hold(_lock);
    RMICBCommand &was = _commands[index];
    for (RMBuffer *read : {was.indices, was.factors, was.patchIndices, was.pointIndices})
        if (read && --_buffers[(__bridge void *)read] == 0)
            _buffers.erase((__bridge void *)read);
    for (RMBuffer *read : {command.indices, command.factors, command.patchIndices, command.pointIndices})
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
    std::lock_guard<raw::Mutex> hold(_lock);
    uint8_t kinds = 0;
    for (uint32_t bit = 0; bit < 4; bit++)
        kinds |= uint8_t((_kinds[bit] ? 1 : 0) << bit);
    return kinds;
}

- (std::vector<RMBuffer *>)buffers
{
    std::lock_guard<raw::Mutex> hold(_lock);
    std::vector<RMBuffer *> out;
    for (const auto &[buffer, uses] : _buffers)
        out.push_back((__bridge RMBuffer *)buffer);
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
    RMIndirectRenderCommand *c = [RMIndirectRenderCommand new];
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
        raw::icb::empty(rmCommandSlot(self, i), _slotWords);
        [self take:RMICBCommand {} at:i];
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

@implementation RMIndirectRenderCommand

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)baseInstance
{
    raw::icb::draw(rmCommandSlot(_buffer, _index), uint32_t(_index),
                   {uint32_t(type), uint32_t(count), uint32_t(instances), uint32_t(start), uint32_t(baseInstance)},
                   _buffer->_slotWords);
    RMICBCommand c;
    c.kind = RMICBCommand::Draw;
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
    RMBuffer *b = [(id)buffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)buffer : nil;
    const uint32_t size = indexType == MTLIndexTypeUInt16 ? 2 : 4;
    if (!b || !b->_raw.va || offset % size || offset >= b->_raw.length) {
        SWITCH_REFUSE("raw", "icb.indices",
                      "indirect command %lu: an index buffer not this device's or off its type (offset %lu)",
                      (unsigned long)_index, (unsigned long)offset);
        [self reset];
        return;
    }
    raw::icb::indexed(rmCommandSlot(_buffer, _index), uint32_t(_index),
                      {uint32_t(type), size, b->_raw.va + offset, b->_raw.va + b->_raw.length - 1, uint32_t(count),
                       uint32_t(instances), 0, int32_t(baseVertex), uint32_t(baseInstance)},
                      _buffer->_slotWords);
    RMICBCommand c;
    c.kind = RMICBCommand::Indexed;
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
    raw::icb::empty(rmCommandSlot(_buffer, _index), _buffer->_slotWords);
    [_buffer take:RMICBCommand {} at:_index];
}

// The buffer inherits these from the encoder (its descriptor's creation said so): Metal's validation refuses them
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
    (void)state;
    SWITCH_REFUSE("raw", "icb.inherited", "an indirect command set a pipeline its buffer inherits");
}

- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)buffer, (void)offset, (void)index;
    SWITCH_REFUSE("raw", "icb.inherited", "an indirect command set a buffer its buffer inherits");
}

- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)buffer, (void)offset, (void)index;
    SWITCH_REFUSE("raw", "icb.inherited", "an indirect command set a buffer its buffer inherits");
}

@end

@interface RMDevice (Indirect)
@end
@implementation RMDevice (Indirect)

- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)d
                                                       maxCommandCount:(NSUInteger)count
                                                               options:(MTLResourceOptions)options
{
    (void)options;
    if (const char *why = unsupportedDescriptor(d)) {
        SWITCH_REFUSE_DETAIL("raw", "icb.descriptor", uintptr_t(why), "indirect command buffer of %s: nil", why);
        return nil;
    }
    if (d.commandTypes & kDispatchTypes)
        return rmNewComputeCommandBuffer(self, d, count);
    RMIndirectCommandBuffer *b = [RMIndirectCommandBuffer new];
    b->_device = self;
    b->_count = count;
    b->_patches = (d.commandTypes & kPatchTypes) != 0;
    b->_slotWords = b->_patches ? raw::icb::kPatchSlotWords : raw::icb::kSlotWords;
    b->_commands.resize(count);
    if (!count || !gRM->raw->createBuffer(uint64_t(count) * b->_slotWords * 4, b->_raw, true)) {
        SWITCH_REFUSE("raw", "icb.memory", "no memory for an indirect command buffer of %lu commands",
                      (unsigned long)count);
        return nil;
    }
    for (NSUInteger i = 0; i < count; i++)
        raw::icb::empty(rmCommandSlot(b, i), b->_slotWords);
    return (id<MTLIndirectCommandBuffer>)b;
}

@end

@implementation RMRenderEncoder (Indirect)

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    [self execute:buffer first:range.location count:range.length range:nil offset:0];
}

// The range the GPU reads at `offset` of `rangeBuffer` (an MTLIndirectCommandBufferExecutionRange) selects the commands:
// every command is referenced, and those outside it draw nothing (raw_mme.h kIcbDraw)
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                 indirectBuffer:(id<MTLBuffer>)rangeBuffer
           indirectBufferOffset:(NSUInteger)offset
{
    RMBuffer *r = [(id)rangeBuffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)rangeBuffer : nil;
    if (!r || (offset & 3) || offset > r->_raw.length || r->_raw.length - offset < 8) {
        SWITCH_REFUSE("raw", "icb.range",
                      "executeCommandsInBuffer:indirectBuffer: a range buffer not this device's or "
                      "outside it: none run");
        return;
    }
    RMIndirectCommandBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)buffer : nil;
    [self execute:buffer first:0 count:b ? b->_count : 0 range:r offset:offset];
}

- (void)execute:(id<MTLIndirectCommandBuffer>)buffer
          first:(NSUInteger)first
          count:(NSUInteger)count
          range:(RMBuffer *)rangeBuffer
         offset:(NSUInteger)offset
{
    SWITCH_API(ApiDraw);
    RMIndirectCommandBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)buffer : nil;
    if (!b || [(id)b isKindOfClass:[RMIndirectComputeBuffer class]] || !count || first + count > b->_count) {
        SWITCH_REFUSE("raw", "icb.range",
                      "executeCommandsInBuffer: a buffer of draws not this device's or a range past it: none run");
        return;
    }
    const uint8_t kinds = [b kinds];
    if ((kinds & RMICBPatches) && (kinds & (RMICBTriangles | RMICBOthers))) {
        [_cb takeReason:WhyEncode why:@"an indirect command buffer of patches and primitives"];
        hit(_cb, PredEncGeometry, 1, kinds);
    } else if ((kinds & (RMICBTriangles | RMICBOthers)) == (RMICBTriangles | RMICBOthers)) {
        [_cb takeReason:WhyEncode why:@"an indirect command buffer of triangles and lines or points"];
        hit(_cb, PredEncGeometry, 0, kinds);
    }
    const std::vector<RMBuffer *> reads = [b buffers];
    [self checkDraw];
    [_cb retain:b];
    [_cb retain:rangeBuffer];
    for (RMBuffer *read : reads)
        [_cb retain:read];
    const uint8_t primitive = kinds & RMICBPatches     ? uint8_t(raw::icb::kPatchPrimitive)
                              : kinds & RMICBTriangles ? 3
                                                       : 0;
    Cmd m = cmd(OpExecuteCommands, primitive);
    m.obj = (__bridge void *)rangeBuffer;
    m.obj2 = (__bridge void *)b;
    m.a = (kinds & RMICBIndexed) != 0;
    m.b = uint32_t(offset);
    m.x = first;
    m.c = count;
    [_cb record:m];
}

@end

bool encodeCommands(raw::RenderEncoder *re, raw::CommandBuffer *rc, const RMPlanDraw &draw, const char **why)
{
    RMIndirectCommandBuffer *b = (__bridge RMIndirectCommandBuffer *)draw.arguments;
    std::vector<const raw::Buffer *> reads;
    for (RMBuffer *buffer : [b buffers]) {
        useBuffer(rc, buffer, false);
        reads.push_back(&buffer->_raw);
    }
    RMBuffer *range = (__bridge RMBuffer *)draw.indices;
    if (range)
        useBuffer(rc, range, false);
    return re->executeSlots({&b->_raw, uint32_t(draw.argumentOffset), uint32_t(draw.indexOffset), b->_slotWords,
                             range ? &range->_raw : nullptr, draw.first, draw.primitive, draw.indexType != 0,
                             b->_gpuWritten.load(std::memory_order_relaxed), reads.data(), reads.size()},
                            why);
}

void replayCommands(id<MTLRenderCommandEncoder> encoder, const RMPlanDraw &draw, const std::function<id(void *)> &use)
{
    RMIndirectCommandBuffer *b = (__bridge RMIndirectCommandBuffer *)draw.arguments;
    if (draw.indices) {
        SWITCH_REFUSE("raw", "icb.replay",
                      "an execute of a range the GPU reads, replayed on m2v: the CPU reads no GPU range, none drawn");
        return;
    }
    std::vector<RMICBCommand> commands;
    {
        std::lock_guard<raw::Mutex> hold(b->_lock);
        commands.assign(b->_commands.begin() + ptrdiff_t(draw.argumentOffset),
                        b->_commands.begin() + ptrdiff_t(draw.argumentOffset + draw.indexOffset));
    }
    for (const RMICBCommand &c : commands)
        if (c.kind == RMICBCommand::Draw)
            [encoder drawPrimitives:MTLPrimitiveType(c.primitive)
                        vertexStart:c.first
                        vertexCount:c.count
                      instanceCount:c.instances
                       baseInstance:c.baseInstance];
        else if (c.kind == RMICBCommand::Indexed)
            [encoder drawIndexedPrimitives:MTLPrimitiveType(c.primitive)
                                indexCount:c.count
                                 indexType:MTLIndexType(c.indexType)
                               indexBuffer:use((__bridge void *)c.indices)
                         indexBufferOffset:c.indexOffset
                             instanceCount:c.instances
                                baseVertex:c.baseVertex
                              baseInstance:c.baseInstance];
}

// Every draw a command of a buffer makes takes the encoder's pipeline (the buffer inherits it), so any render state draws
// one
@implementation RMPipeline (Indirect)

- (BOOL)supportIndirectCommandBuffers
{
    return YES;
}

@end
