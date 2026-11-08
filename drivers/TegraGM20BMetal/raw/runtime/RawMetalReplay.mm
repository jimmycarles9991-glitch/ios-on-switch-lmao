// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

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

// An invocation recorded on a facade encoder or command buffer, sent to the m2v one: object arguments become their twins.
static void replayInvocation(NSInvocation *inv, id target)
{
    raw::invokeMapped(inv, target, [](id o) {
        return twinOf(o);
    });
}

// The tape into an m2v command buffer, every object its twin; the CPU lines of the buffers it names cleaned first.
// The tape [from, to) of `cb` as an m2v command buffer, not committed. `part`: a segment of a split command buffer (planSegments),
// whose encoders' waits for fences another segment updated are left out: the segments' order on the GPU covers them.
id<MTLCommandBuffer> replay(RMCommandBuffer *cb, size_t from, size_t to, bool part)
{
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
        const Cmd &c = cb->_tape[at];
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
        case OpPipeline:
            pipe = [(__bridge id)c.obj isKindOfClass:[RMPipeline class]] ? (__bridge RMPipeline *)c.obj : nil;
            [enc setRenderPipelineState:twinOf((__bridge id)c.obj)];
            break;
        case OpBytes:
            if (c.stage <= 2 && c.index <= 30)
                bound[c.stage][c.index] = nil;
            if (c.stage == 1)
                [enc setVertexBytes:cb->_bytes.chunks[c.c].cpu + c.a length:c.b atIndex:c.index];
            else
                [enc setFragmentBytes:cb->_bytes.chunks[c.c].cpu + c.a length:c.b atIndex:c.index];
            break;
        case OpBuffer: {
            id x = (__bridge id)c.obj;
            if (c.stage <= 2 && c.index <= 30) {
                bound[c.stage][c.index] = [x isKindOfClass:[RMBuffer class]] ? (RMBuffer *)x : nil;
                boundOffset[c.stage][c.index] = c.c;
            }
            if (c.stage == 1)
                [enc setVertexBuffer:twinOf(x) offset:c.c atIndex:c.index];
            else
                [enc setFragmentBuffer:twinOf(x) offset:c.c atIndex:c.index];
            break;
        }
        case OpBufferOffset:
            if (c.stage <= 2 && c.index <= 30)
                boundOffset[c.stage][c.index] = c.c;
            if (c.stage == 1)
                [enc setVertexBufferOffset:c.c atIndex:c.index];
            else
                [enc setFragmentBufferOffset:c.c atIndex:c.index];
            break;
        case OpTexture:
            if (c.stage == 1)
                [enc setVertexTexture:twinOf((__bridge id)c.obj) atIndex:c.index];
            else
                [enc setFragmentTexture:twinOf((__bridge id)c.obj) atIndex:c.index];
            break;
        case OpSampler:
            if (c.stage == 1)
                [enc setVertexSamplerState:twinOf((__bridge id)c.obj) atIndex:c.index];
            else
                [enc setFragmentSamplerState:twinOf((__bridge id)c.obj) atIndex:c.index];
            break;
        case OpScissor:
            [enc setScissorRect:(MTLScissorRect){c.a, c.b, NSUInteger(c.c >> 32), NSUInteger(uint32_t(c.c))}];
            break;
        case OpViewport:
            [enc setViewport:(MTLViewport){extraDouble(cb, c.x), extraDouble(cb, c.x + 1), extraDouble(cb, c.x + 2),
                                           extraDouble(cb, c.x + 3), extraDouble(cb, c.x + 4),
                                           extraDouble(cb, c.x + 5)}];
            break;
        case OpDraw:
            drawn(c.a, c.b);
            [enc drawPrimitives:MTLPrimitiveType(c.stage)
                    vertexStart:c.a
                    vertexCount:c.b
                  instanceCount:NSUInteger(c.c >> 32)
                   baseInstance:NSUInteger(uint32_t(c.c))];
            break;
        case OpCull:
            [enc setCullMode:MTLCullMode(c.a)];
            break;
        case OpWinding:
            [enc setFrontFacingWinding:MTLWinding(c.a)];
            break;
        case OpDepthStencil:
            [enc setDepthStencilState:twinOf((__bridge id)c.obj)];
            break;
        case OpFillMode:
            [enc setTriangleFillMode:MTLTriangleFillMode(c.a)];
            break;
        case OpBlendColor:
            [enc setBlendColorRed:extraFloat(cb, c.x)
                            green:extraFloat(cb, c.x + 1)
                             blue:extraFloat(cb, c.x + 2)
                            alpha:extraFloat(cb, c.x + 3)];
            break;
        case OpStencilRef:
            [enc setStencilFrontReferenceValue:c.a backReferenceValue:c.b];
            break;
        case OpDepthBias:
            [enc setDepthBias:extraFloat(cb, c.x) slopeScale:extraFloat(cb, c.x + 1) clamp:extraFloat(cb, c.x + 2)];
            break;
        case OpBarrier:
            [enc memoryBarrierWithScope:MTLBarrierScope(c.a)
                            afterStages:MTLRenderStages(c.b)
                           beforeStages:MTLRenderStages(c.c)];
            break;
        case OpUse:
            [enc useResource:use(c.obj) usage:MTLResourceUsage(c.a) stages:MTLRenderStages(c.b)];
            break;
        case OpUpdateFence:
            updated.insert(c.obj);
            [enc updateFence:(__bridge id<MTLFence>)c.obj];
            break;
        case OpWaitFence:
            if (!part || updated.count(c.obj))
                [enc waitForFence:(__bridge id<MTLFence>)c.obj];
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
                    range(b, extra(cb, x), extra(cb, x + 1) * (rows - 1) + extra(cb, x + 3) * t->_raw.bpp);
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
                    range(b, extra(cb, x + 7), extra(cb, x + 8) * (rows - 1) + extra(cb, x + 4) * t->_raw.bpp, true);
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
            break;
        }
        case OpSignalEvent:
            [m encodeSignalEvent:(__bridge id<MTLEvent>)c.obj value:c.c];
            break;
        case OpWaitEvent:
            // a value raw work submitted before signals: the BeforeTask acquire orders this command buffer after it on the
            // GPU (the raw path signals its events from the CPU once complete, which m2v would wait for on the CPU)
            if (!(gRM->gpuOrder && signalled(gRM->rawSignals, c.obj) >= c.c))
                [m encodeWaitForEvent:(__bridge id<MTLEvent>)c.obj value:c.c];
            break;
        case OpDrawIndexed: {
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
                const int64_t base = int64_t(extra(cb, c.x + 1)), from = std::max<int64_t>(0, base);
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
        }
            [enc drawIndexedPrimitives:MTLPrimitiveType(c.stage)
                            indexCount:c.a
                             indexType:MTLIndexType(c.b)
                           indexBuffer:twinOf((__bridge id)c.obj)
                     indexBufferOffset:c.c
                         instanceCount:extra(cb, c.x)
                            baseVertex:NSInteger(extra(cb, c.x + 1))
                          baseInstance:extra(cb, c.x + 2)];
            break;
        case OpVisibility:
            [enc setVisibilityResultMode:MTLVisibilityResultMode(c.a) offset:c.c];
            break;
        }
    }
    if (enc)
        [enc endEncoding];
    // what the CPU wrote that it reads, to memory (above): the ranges, then the whole buffers
    uint64_t cleaned = 0;
    for (const Range &r : ranges) {
#if defined(__aarch64__)
        for (uintptr_t a = uintptr_t(r.at) & ~uintptr_t(63); a < uintptr_t(r.at) + r.bytes; a += 64) {
            if (r.flush)
                __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
            else
                __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
        }
#endif
        cleaned += r.bytes;
    }
    for (RMBuffer *b : buffers) {
        if (!b || b->_raw.wc)
            continue;
#if defined(__aarch64__)
        for (uintptr_t a = uintptr_t(b->_raw.cpu) & ~uintptr_t(63); a < uintptr_t(b->_raw.cpu) + b->_raw.length;
             a += 64)
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
#endif
        cleaned += b->_raw.length;
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    gRM->stats.replayClean += cleaned;
#if SWITCH_FRAME_TIMELINE
    gRM->stats.replayDraws +=
        uint64_t(std::count_if(cb->_tape.begin() + from, cb->_tape.begin() + to, [](const Cmd &c) {
            return c.op == OpDraw || c.op == OpDrawIndexed;
        }));
#endif
    noteSurfaceWrites(cb, from, to);
    return m;
}
