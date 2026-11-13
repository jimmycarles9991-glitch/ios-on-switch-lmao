// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#import "RawMetalCounters.h"
#import "RawMetalMultisample.h"
#import <objc/message.h>
#import "RawMetalPlanAdapters.h"
#import "RawMetalIndirect.h"
#include "NativeBridgeMetalFrontend.h"
#include "raw_replay_twins.h"

// ---- object mapping for the m2v replay ----
#if SWITCH_FRAME_TIMELINE
thread_local uint64_t tTwinTicks;

void countTwin(id object, uint64_t ticks)
{
    Stats &s = gRM->stats;
    if ([object isKindOfClass:[RMTexture class]]) {
        s.twinTextures++;
        s.twinTextureUs += ticks * 1000 / switch_frame::ticksPerMs();
    } else {
        ([object isKindOfClass:[RMBuffer class]] ? s.twinBuffers : s.twinOthers)++;
    }
}
#endif

static MTLRenderPassDescriptor *twinPass(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDescriptor *d = [pass copy];
    for (unsigned c = 0; c < 8; c++) {
        d.colorAttachments[c].texture = twinOf(pass.colorAttachments[c].texture);
        d.colorAttachments[c].resolveTexture = twinOf(pass.colorAttachments[c].resolveTexture);
    }
    d.depthAttachment.texture = twinOf(pass.depthAttachment.texture);
    d.depthAttachment.resolveTexture = twinOf(pass.depthAttachment.resolveTexture);
    d.stencilAttachment.texture = twinOf(pass.stencilAttachment.texture);
    d.stencilAttachment.resolveTexture = twinOf(pass.stencilAttachment.resolveTexture);
    d.visibilityResultBuffer = twinOf(pass.visibilityResultBuffer);
    return d;
}

template <typename Visit> static void forEachAttachment(MTLRenderPassDescriptor *pass, Visit &&visit)
{
    for (unsigned c = 0; c < 8; c++) {
        visit((__bridge void *)pass.colorAttachments[c].texture);
        visit((__bridge void *)pass.colorAttachments[c].resolveTexture);
    }
    visit((__bridge void *)pass.depthAttachment.texture);
    visit((__bridge void *)pass.depthAttachment.resolveTexture);
    visit((__bridge void *)pass.stencilAttachment.texture);
    visit((__bridge void *)pass.stencilAttachment.resolveTexture);
    visit((__bridge void *)pass.visibilityResultBuffer);
}

static void *missingAttachmentTwin(RMCommandBuffer *cb, size_t from, size_t to)
{
    return raw::missingTwin(
        from, to,
        [cb](size_t at, auto &&visit) {
            if (cb->_plan.operation(at).op == OpRender)
                forEachAttachment((__bridge MTLRenderPassDescriptor *)cb->_plan.legacy(at).obj, visit);
        },
        [](void *object) {
            return twinOf((__bridge id)object) != nil;
        });
}

// The command buffer's error for a range replay could not encode: `object`, an attachment with no m2v twin, named
static void failMissingTwin(RMCommandBuffer *cb, id object)
{
    RMTexture *t = [object isKindOfClass:[RMTexture class]] ? (RMTexture *)object : nil;
    const unsigned long format = t ? (unsigned long)t->_descriptor.pixelFormat : 0;
    const char *label = [object label].UTF8String ?: "-";
    SWITCH_REFUSE_DETAIL("raw", "replay.attachment", format,
                         "a command buffer range failed before m2v encoded it: its attachment %s (%ux%u, format %lu) "
                         "has no m2v twin",
                         label, t ? t->_raw.width : 0, t ? t->_raw.height : 0, format);
    NSString *text = [NSString
        stringWithFormat:@"The render pass attachment %s (%ux%u, pixel format %lu) has no compatibility texture", label,
                         t ? t->_raw.width : 0, t ? t->_raw.height : 0, format];
    NSError *error = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                         code:MTLCommandBufferErrorInternal
                                     userInfo:@{NSLocalizedDescriptionKey : text}];
    std::lock_guard<raw::Mutex> l(cb->_lock);
    if (!cb->_segError)
        cb->_segError = error;
}

static bool takesArgumentBuffers(id<MTLFunction> f)
{
    if (![(id)f isKindOfClass:[RMFunction class]])
        return false;
    RMFunction *function = (RMFunction *)f;
    if (!function->_argumentBuffers) {
        id twin = function->_twin ?: [function twinOn:RM_M2V("a replay's argument-buffer check")];
        bool takes = false;
        for (NSUInteger index = 0; index < 31 && !takes; index++) {
            const char *why = nullptr;
            takes = native_bridge_metal_argument_layouts(
                twin, index,
                [](const NmbArgumentLayout *, uint32_t, void *) {
                    return true;
                },
                nullptr, &why);
        }
        function->_argumentBuffers = takes ? 2 : 1;
    }
    return function->_argumentBuffers == 2;
}

// Whether a render state reads through argument buffers: its record names the device's root table, status buffer, handle
// tables or set 1, or (a state with no record yet) one of its functions takes an argument buffer
static bool readsArguments(RMPipeline *p)
{
    if (p->_raw && p->_raw->rec) {
        const raw::PackPipeline &r = *p->_raw->rec;
        bool tables = false;
        for (uint32_t i = 0; i < r.entries; i++)
            tables |= r.entry[i].kind == raw::EntryTextureTable || r.entry[i].kind == raw::EntrySamplerTable;
        return tables || r.heapArrays || r.rootTableWord[0] || r.rootTableWord[1] || r.statusWord[0] || r.statusWord[1];
    }
    MTLRenderPipelineDescriptor *d = p->_madeFrom;
    return d && (takesArgumentBuffers(d.vertexFunction) || takesArgumentBuffers(d.fragmentFunction));
}

// What a range reads that m2v's twins cannot (raw_replay_twins.h): a buffer RawMetalArgumentEncoder.mm wrote, or a facade
// heap beside a render state that reads argument buffers, whose IDs the app took of the raw device (gpuResourceID)
static id rawArgumentsIn(RMCommandBuffer *cb, size_t from, size_t to)
{
    return (__bridge id)raw::rawArgumentsIn(
        [&](auto &&visit) {
            cb->_plan.forEachUse(from, to, [&](void *value, uint8_t) {
                visit(value);
            });
        },
        [](void *value) {
            id o = (__bridge id)value;
            if ([o isKindOfClass:[RMHeap class]])
                return raw::ReplayUse::Heap;
            if ([o isKindOfClass:[RMBuffer class]] && ((RMBuffer *)o)->_rawArguments)
                return raw::ReplayUse::WrittenArguments;
            return raw::ReplayUse::Other;
        },
        [&](auto &&visit) {
            for (size_t at = from; at < to; at++)
                if (const Cmd c = cb->_plan.legacy(at); c.op == OpPipeline)
                    visit(c.obj);
        },
        [](void *state) {
            id s = (__bridge id)state;
            return [s isKindOfClass:[RMPipeline class]] && readsArguments((RMPipeline *)s);
        });
}

// The command buffer's error for a range replay could not draw as Metal does: it reads through the raw device's argument
// buffers (rawArgumentsIn)
static void failRawArguments(RMCommandBuffer *cb, id object)
{
    const bool heap = [object isKindOfClass:[RMHeap class]];
    const char *label = [object label].UTF8String ?: "-";
    SWITCH_REFUSE_DETAIL("raw", "replay.arguments", heap,
                         "a command buffer range failed before m2v encoded it: it reads the raw device's %s %s, which "
                         "no m2v twin reads",
                         heap ? "heap" : "argument buffer", label);
    NSString *text =
        [NSString stringWithFormat:@"The %s %s is read through argument buffers the compatibility path cannot read",
                                   heap ? "heap" : "argument buffer", label];
    NSError *error = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                         code:MTLCommandBufferErrorInternal
                                     userInfo:@{NSLocalizedDescriptionKey : text}];
    std::lock_guard<raw::Mutex> l(cb->_lock);
    if (!cb->_segError)
        cb->_segError = error;
}

// An invocation recorded on a facade encoder or command buffer, sent to the m2v one: object arguments become their twins.
static void replayInvocation(NSInvocation *inv, id target)
{
    raw::invokeMapped(inv, target, [](id o) {
        return twinOf(o);
    });
}

// The tape into an m2v command buffer, every object its twin; the CPU lines of the buffers it names cleaned first.
// The tape [from, to) of `cb` as an m2v command buffer, not committed. `part`: a segment of a split command buffer (planSegments),
// whose encoders' waits for fences another segment updated are left out: the segments' order on the GPU covers them. Nil, with
// nothing encoded and the command buffer's error set, where an attachment of one of its passes has no twin.
id<MTLCommandBuffer> replay(RMCommandBuffer *cb, size_t from, size_t to, bool part)
{
    if (rmRefuseM2vRange(cb, from, to))
        return nil;
    if (void *missing = missingAttachmentTwin(cb, from, to)) {
        failMissingTwin(cb, (__bridge id)missing);
        return nil;
    }
    if (RMTexture *owned = ownedZetaIn(cb, from, to)) {
        failOwnedZeta(cb, owned);
        return nil;
    }
    if (id arguments = rawArgumentsIn(cb, from, to)) {
        failRawArguments(cb, arguments);
        return nil;
    }
    rmCounterMarksDropped(cb, from, to);
    id<MTLCommandQueue> queue = [cb->_queue rmTwin];
#if SWITCH_FRAME_TIMELINE
    cb->_frame.mark(switch_frame::SlotStart);
#endif
    id<MTLCommandBuffer> m = [queue commandBuffer];
#if SWITCH_FRAME_TIMELINE
    cb->_frame.mark(switch_frame::SlotEnd);
#endif
    if (!m)
        return nil;
    m.label = cb.label;
    cb->_queue->_residency.useOn(m);
    cb->_residency.useOn(m);
    id enc = nil;
    // What the CPU wrote that this work reads, cleaned to memory before m2v's submission (m2v cleans an import only when it
    // makes it): the ranges a draw of a pack state reads, as raw::RenderEncoder::prepare cleans them (its uniform windows, its
    // vertices, its indices), the ranges a copy reads (and writes: cleaned and invalidated, so no dirty line lands over the
    // GPU's bytes); a whole buffer only where the tape does not say (a state not in the pack, a compute or other recorded call,
    // a resource it uses)
    std::set<RMBuffer *> buffers;  // whole
    std::set<RMBuffer *> writtenBuffers;
    cb->_plan.forEachUse(from, to, [&](void *value, uint8_t access) {
        RMBuffer *buffer = cpuBufferOfResource((__bridge id)value);
        if (!buffer || buffer->_raw.wc)
            return;
        if (access & raw::ResourceUse::Write) {
            buffers.insert(buffer);
            writtenBuffers.insert(buffer);
        } else if (access & raw::ResourceUse::Explicit) {
            buffers.insert(buffer);
        }
    });

    struct Range {
        const uint8_t *at;
        uint64_t bytes;
        bool flush;
    };

    std::vector<Range> ranges;
    auto range = [&](RMBuffer *b, uint64_t offset, uint64_t bytes, bool flush = false) {
        if (![(id)b isKindOfClass:[RMBuffer class]] || offset >= b->_raw.length || b->_raw.wc)
            return;  // (write-combined: in memory once the DSB below drains it)
        ranges.push_back({b->_raw.cpu + offset, std::min(bytes, b->_raw.length - offset), flush});
    };
    auto use = [&](void *o) -> id {
        id x = (__bridge id)o;
        if ([x isKindOfClass:[RMBuffer class]])
            buffers.insert((RMBuffer *)x);
        return twinOf(x);
    };
    // a render encoder's bindings, for its draws' ranges
    RMBuffer *bound[3][31] = {};
    uint64_t boundOffset[3][31] = {};
    RMPipeline *pipe = nil;
    raw::StateCursor stateCursor;
    auto drawn = [&](uint32_t first, uint32_t count) {  // the ranges a draw of `count` vertices from `first` reads
        if (!pipe || !pipe->_raw) {                     // not the pack's: every buffer bound, whole
            for (auto &stage : bound)
                for (RMBuffer *b : stage)
                    if (b)
                        buffers.insert(b);
            return;
        }
        const raw::PackPipeline &r = *pipe->_raw->rec;
        for (uint32_t i = 0; i < r.entries; i++) {
            const raw::PackEntry &e = r.entry[i];
            if (e.kind != raw::EntryUbo || e.stage > 2 || e.slot > 30)
                continue;
            RMBuffer *b = bound[e.stage][e.slot];
            if (!b)
                continue;
            const uint64_t at = boundOffset[e.stage][e.slot], size = b->_raw.length - std::min(at, b->_raw.length);
            const uint64_t window = (uint32_t(e.sampler) | uint32_t(e.pad) << 8) * 16;
            uint64_t bytes = std::min<uint64_t>((size + 15) & ~uint64_t(15), 0x10000);
            if (window && window < bytes)
                bytes = window;
            range(b, at, bytes);
        }
        if (r.vertexSlot <= 30 && r.vertexStride)
            if (RMBuffer *b = bound[1][r.vertexSlot])
                range(b, boundOffset[1][r.vertexSlot] + uint64_t(first) * r.vertexStride,
                      uint64_t(count) * r.vertexStride);
    };
    std::set<void *> updated;  // fences this part updated
    for (size_t at = from; at < to; at++) {
        const Cmd c = cb->_plan.legacy(at);
        replayPlannedState(cb, at, enc, stateCursor, buffers, bound, boundOffset, pipe);
        if (c.op == OpDispatch) {
            const auto &dispatch = cb->_plan.dispatch(at);
            const MTLSize grid = MTLSizeMake(dispatch.grid[0], dispatch.grid[1], dispatch.grid[2]);
            const MTLSize threads = MTLSizeMake(dispatch.threads[0], dispatch.threads[1], dispatch.threads[2]);
            const auto &in = dispatch.stageIn;
            [enc setStageInRegion:MTLRegionMake3D(in[0], in[1], in[2], in[3], in[4], in[5])];
            if (dispatch.perThread)
                [enc dispatchThreads:grid threadsPerThreadgroup:threads];
            else
                [enc dispatchThreadgroups:grid threadsPerThreadgroup:threads];
            continue;
        }
        if (isKernelCommand(c.op)) {
            replayKernel(enc, cb, c, buffers);
            continue;
        }
        switch (c.op) {
        case OpRender: {
            enc = [m renderCommandEncoderWithDescriptor:twinPass((__bridge MTLRenderPassDescriptor *)c.obj)];
            memset(bound, 0, sizeof bound);
            pipe = nil;
            break;
        }
        case OpBlit:
            enc = [m blitCommandEncoder];
            break;
        case OpCompute:
            enc = [m computeCommandEncoderWithDispatchType:MTLDispatchType(c.a)];
            break;
        case OpEnd:
            [enc endEncoding];
            enc = nil;
            break;
        case OpDraw:
            drawn(c.a, c.b);
            [enc drawPrimitives:MTLPrimitiveType(c.stage)
                    vertexStart:c.a
                    vertexCount:c.b
                  instanceCount:NSUInteger(c.c >> 32)
                   baseInstance:NSUInteger(uint32_t(c.c))];
            break;
        case OpBarrier:
            [enc memoryBarrierWithScope:MTLBarrierScope(c.a)
                            afterStages:MTLRenderStages(c.b)
                           beforeStages:MTLRenderStages(c.c)];
            break;
        case OpUse:
            // a range that uses a facade heap fails before it is encoded (rawArgumentsIn)
            if (![(__bridge id)c.obj isKindOfClass:[RMHeap class]])
                [enc useResource:use(c.obj) usage:MTLResourceUsage(c.a) stages:MTLRenderStages(c.b)];
            break;
        case OpUpdateFence:
            updated.insert(c.obj);
            [enc updateFence:twinOf((__bridge id)c.obj)];
            break;
        case OpWaitFence:
            if (!part || updated.count(c.obj))
                [enc waitForFence:twinOf((__bridge id)c.obj)];
            break;
        case OpCopyTexture: {
            const uint64_t x = c.x, o = extra(cb, x + 10);
            [enc copyFromTexture:twinOf((__bridge id)c.obj)
                      sourceSlice:extra(cb, x)
                      sourceLevel:extra(cb, x + 1)
                     sourceOrigin:MTLOriginMake(extra(cb, x + 2), extra(cb, x + 3), extra(cb, x + 4))
                       sourceSize:MTLSizeMake(extra(cb, x + 5), extra(cb, x + 6), extra(cb, x + 7))
                        toTexture:twinOf((__bridge id)c.obj2)
                 destinationSlice:extra(cb, x + 8)
                 destinationLevel:extra(cb, x + 9)
                destinationOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 11))];
            break;
        }
        case OpFillTexture: {
            float rgba[4];
            for (int k = 0; k < 4; k++) {
                const uint32_t bits = uint32_t(extra(cb, c.x + 4 + k));
                memcpy(&rgba[k], &bits, sizeof bits);
            }
            // an SPI selector the SDK's protocol does not declare
            using Fill = void (*)(id, SEL, id, NSUInteger, NSUInteger, MTLRegion, MTLClearColor);
            reinterpret_cast<Fill>(objc_msgSend)(
                enc, NSSelectorFromString(@"fillTexture:level:slice:region:color:"), twinOf((__bridge id)c.obj), 0, 0,
                MTLRegionMake2D(extra(cb, c.x), extra(cb, c.x + 1), extra(cb, c.x + 2), extra(cb, c.x + 3)),
                MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]));
            break;
        }
        case OpFillBuffer:
            range((__bridge RMBuffer *)c.obj, c.c, extra(cb, c.x), true);
            [enc fillBuffer:twinOf((__bridge id)c.obj)
                      range:NSMakeRange(NSUInteger(c.c), NSUInteger(extra(cb, c.x)))
                      value:uint8_t(c.a)];
            break;
        case OpCopyBuffer:
            range((__bridge RMBuffer *)c.obj, c.c, extra(cb, c.x + 1));
            range((__bridge RMBuffer *)c.obj2, extra(cb, c.x), extra(cb, c.x + 1), true);
            [enc copyFromBuffer:twinOf((__bridge id)c.obj)
                     sourceOffset:c.c
                         toBuffer:twinOf((__bridge id)c.obj2)
                destinationOffset:extra(cb, c.x)
                             size:extra(cb, c.x + 1)];
            break;
        case OpCopyBufferToTexture: {
            const uint64_t x = c.x, o = extra(cb, x + 8);
            {  // the rows it reads: bytesPerRow apart, the last one's texels (a whole buffer where the texture is not the raw
                // path's: its texel size unknown here)
                RMBuffer *b = (__bridge RMBuffer *)c.obj;
                RMTexture *t = (__bridge RMTexture *)c.obj2;
                const uint64_t rows = extra(cb, x + 4) * std::max<uint64_t>(extra(cb, x + 5), 1);
                if ([(id)b isKindOfClass:[RMBuffer class]] && [(id)t isKindOfClass:[RMTexture class]] && t->_raw.bpp &&
                    rows)
                    range(b, extra(cb, x),
                          extra(cb, x + 1) * (rows - 1) + extra(cb, x + 3) * raw::bufferUnit(t->_raw, uint8_t(c.a)));
                else
                    use(c.obj);
            }
            [enc copyFromBuffer:twinOf((__bridge id)c.obj)
                       sourceOffset:extra(cb, x)
                  sourceBytesPerRow:extra(cb, x + 1)
                sourceBytesPerImage:extra(cb, x + 2)
                         sourceSize:MTLSizeMake(extra(cb, x + 3), extra(cb, x + 4), extra(cb, x + 5))
                          toTexture:twinOf((__bridge id)c.obj2)
                   destinationSlice:extra(cb, x + 6)
                   destinationLevel:extra(cb, x + 7)
                  destinationOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 9))];
            break;
        }
        case OpCopyTextureToBuffer: {
            const uint64_t x = c.x, o = extra(cb, x + 2);
            {  // the rows it writes, cleaned and invalidated (a whole buffer where the texel size is unknown here)
                RMTexture *t = (__bridge RMTexture *)c.obj;
                RMBuffer *b = (__bridge RMBuffer *)c.obj2;
                const uint64_t rows = extra(cb, x + 5) * std::max<uint64_t>(extra(cb, x + 6), 1);
                if ([(id)b isKindOfClass:[RMBuffer class]] && [(id)t isKindOfClass:[RMTexture class]] && t->_raw.bpp &&
                    rows)
                    range(b, extra(cb, x + 7),
                          extra(cb, x + 8) * (rows - 1) + extra(cb, x + 4) * raw::bufferUnit(t->_raw, uint8_t(c.a)),
                          true);
                else
                    use(c.obj2);
            }
            [enc copyFromTexture:twinOf((__bridge id)c.obj)
                             sourceSlice:extra(cb, x)
                             sourceLevel:extra(cb, x + 1)
                            sourceOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 3))
                              sourceSize:MTLSizeMake(extra(cb, x + 4), extra(cb, x + 5), extra(cb, x + 6))
                                toBuffer:use(c.obj2)
                       destinationOffset:extra(cb, x + 7)
                  destinationBytesPerRow:extra(cb, x + 8)
                destinationBytesPerImage:extra(cb, x + 9)];
            break;
        }
        case OpMipmaps:
            [enc generateMipmapsForTexture:twinOf((__bridge id)c.obj)];
            break;
        case OpInvocation: {
            NSInvocation *inv = (__bridge NSInvocation *)c.obj;
            raw::forEachObject(inv, [&](id o) {  // buffers it names get cleaned too
                if ([o isKindOfClass:[RMBuffer class]])
                    buffers.insert((RMBuffer *)o);
            });
            replayInvocation(inv, c.stage == 0xff ? m : enc);
            pipe = nil;
            break;
        }
        case OpSignalEvent:
            [m encodeSignalEvent:(__bridge id<MTLEvent>)c.obj value:c.c];
            break;
        case OpWaitEvent:
            if (expiredEventWait(cb, at))
                break;
            [m encodeWaitForEvent:(__bridge id<MTLEvent>)c.obj value:c.c];
            break;
        case OpDrawIndexed: {
            const auto &draw = cb->_plan.draw(at);
            // its indices, and the vertices up to the largest (a restart reads none), as the raw path's indexed draw cleans
            RMBuffer *ib = [(__bridge id)c.obj isKindOfClass:[RMBuffer class]] ? (__bridge RMBuffer *)c.obj : nil;
            const uint32_t size = c.b == MTLIndexTypeUInt16 ? 2 : 4;
            if (ib && pipe && pipe->_raw && c.c + uint64_t(c.a) * size <= ib->_raw.length) {
                range(ib, c.c, uint64_t(c.a) * size);
                // the largest index only sizes the vertices to clean: none in write-combined memory (r17), whose indices the
                // CPU would read uncached, one load each (raw::RenderEncoder::drawIndexed's rule)
                const raw::PackPipeline &rec = *pipe->_raw->rec;
                RMBuffer *vb = rec.vertexSlot <= 30 ? bound[1][rec.vertexSlot] : nil;
                uint32_t most = 0;
                if (vb && !vb->_raw.wc && rec.vertexStride)
                    for (uint32_t k = 0; k < c.a; k++) {
                        uint32_t v = 0;
                        memcpy(&v, ib->_raw.cpu + c.c + uint64_t(k) * size, size);
                        if (v != (size == 2 ? 0xffffu : 0xffffffffu))
                            most = std::max(most, v);
                    }
                // from the base vertex: those a negative one puts before the stream are none
                const int64_t base = int64_t(draw.baseVertex), from = std::max<int64_t>(0, base);
                const int64_t end = c.a ? std::max<int64_t>(from, base + most + 1) : from;
                drawn(uint32_t(from), uint32_t(end - from));
            } else {
                if (ib)
                    buffers.insert(ib);
                for (auto &stage : bound)
                    for (RMBuffer *b : stage)
                        if (b)
                            buffers.insert(b);
            }
            [enc drawIndexedPrimitives:MTLPrimitiveType(c.stage)
                            indexCount:c.a
                             indexType:MTLIndexType(c.b)
                           indexBuffer:twinOf((__bridge id)c.obj)
                     indexBufferOffset:c.c
                         instanceCount:draw.instances
                            baseVertex:NSInteger(draw.baseVertex)
                          baseInstance:draw.baseInstance];
            break;
        }
        case OpDrawIndirect:
        case OpDrawIndexedIndirect: {
            const auto &draw = cb->_plan.draw(at);
            for (auto &stage : bound)
                for (RMBuffer *buffer : stage)
                    if (buffer)
                        buffers.insert(buffer);
            id arguments = use(draw.arguments);
            if (c.op == OpDrawIndirect)
                [enc drawPrimitives:MTLPrimitiveType(draw.primitive)
                          indirectBuffer:arguments
                    indirectBufferOffset:draw.argumentOffset];
            else
                [enc drawIndexedPrimitives:MTLPrimitiveType(draw.primitive)
                                 indexType:MTLIndexType(draw.indexType)
                               indexBuffer:use(draw.indices)
                         indexBufferOffset:draw.indexOffset
                            indirectBuffer:arguments
                      indirectBufferOffset:draw.argumentOffset];
            break;
        }
        case OpExecuteCommands:
            for (auto &stage : bound)
                for (RMBuffer *buffer : stage)
                    if (buffer)
                        buffers.insert(buffer);
            replayCommands(enc, cb->_plan.draw(at), use);
            break;
        case OpDrawPatches:
        case OpDrawPatchesIndirect:
            for (auto &stage : bound)
                for (RMBuffer *buffer : stage)
                    if (buffer)
                        buffers.insert(buffer);
            replayPatches(enc, cb->_plan.draw(at), cb->_plan.patches(at), c.op == OpDrawPatchesIndirect, use);
            break;
        case OpResetCommands:
        case OpCopyCommands:
            encodeCommandsBlit(nullptr, c);
            break;
        default:  // a compute encoder's: replayKernel, above
            break;
        }
    }
    if (enc)
        [enc endEncoding];
    // what the CPU wrote that it reads, to memory (above): the ranges, then the whole buffers
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t cleaned = 0;
#endif
    for (const Range &r : ranges) {
#if defined(__aarch64__)
        for (uintptr_t a = uintptr_t(r.at) & ~uintptr_t(63); a < uintptr_t(r.at) + r.bytes; a += 64) {
            if (r.flush)
                __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
            else
                __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
        }
#endif
        RAW_DIAGNOSTIC(cleaned += r.bytes);
    }
    for (RMBuffer *b : buffers) {
        if (!b || b->_raw.wc)
            continue;
#if defined(__aarch64__)
        const bool written = writtenBuffers.count(b) != 0;
        for (uintptr_t a = uintptr_t(b->_raw.cpu) & ~uintptr_t(63); a < uintptr_t(b->_raw.cpu) + b->_raw.length;
             a += 64)
            if (written)
                __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
            else
                __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
#endif
        RAW_DIAGNOSTIC(cleaned += b->_raw.length);
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    RAW_DIAGNOSTIC(gRM->stats.replayClean += cleaned);
#if SWITCH_FRAME_TIMELINE
    for (size_t at = from; at < to; ++at) {
        const Op op = cb->_plan.operation(at).op;
        RAW_DIAGNOSTIC(gRM->stats.replayDraws += isPlanDraw(op));
    }
#endif
    noteSurfaceWrites(cb, from, to);
    return m;
}
