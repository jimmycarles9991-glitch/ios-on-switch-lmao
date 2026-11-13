// SPDX-License-Identifier: MIT
#import "RawMetalPlanAdapters.h"
#import "RawMetalCounters.h"
#include "TegraGM20BClockABI.h"

namespace {

struct ReplayPlanSink {
    RMCommandBuffer *cb;
    id encoder;
    std::set<RMBuffer *> &buffers;
    RMBuffer *__strong (*bound)[31];
    uint64_t (*offsets)[31];
    RMPipeline *__strong &pipeline;

    void buffer(uint32_t stage, uint32_t slot, const raw::BufferBinding &binding)
    {
        if (binding.stride != raw::kStaticStride && (stage == 1 || stage == 3)) {
            stridedBuffer(stage, slot, binding);
            return;
        }
        if (binding.source == raw::BufferBinding::Source::Offset) {
            if (stage < 3)
                offsets[stage][slot] = binding.offset;
            if (stage == 1)
                [encoder setVertexBufferOffset:binding.offset atIndex:slot];
            else if (stage == 2)
                [encoder setFragmentBufferOffset:binding.offset atIndex:slot];
            else
                [encoder setBufferOffset:binding.offset atIndex:slot];
            return;
        }
        if (binding.source == raw::BufferBinding::Source::ClockAnchors) {
            const TegraGM20BAbiClockAnchors anchors = rmClockAnchorsNow();
            [encoder setBytes:&anchors length:sizeof anchors atIndex:slot];
            return;
        }
        if (binding.source == raw::BufferBinding::Source::Bytes) {
            const void *bytes = cb->_bytes.chunks[binding.chunk].cpu + binding.offset;
            if (stage < 3)
                bound[stage][slot] = nil;
            if (stage == 1)
                [encoder setVertexBytes:bytes length:binding.length atIndex:slot];
            else if (stage == 2)
                [encoder setFragmentBytes:bytes length:binding.length atIndex:slot];
            else
                [encoder setBytes:bytes length:binding.length atIndex:slot];
            return;
        }
        id object = (__bridge id)binding.object;
        RMBuffer *buffer = [object isKindOfClass:[RMBuffer class]] ? object : nil;
        if (stage < 3) {
            bound[stage][slot] = buffer;
            offsets[stage][slot] = binding.offset;
        } else if (buffer) {
            buffers.insert(buffer);
        }
        if (stage == 1)
            [encoder setVertexBuffer:twinOf(object) offset:binding.offset atIndex:slot];
        else if (stage == 2)
            [encoder setFragmentBuffer:twinOf(object) offset:binding.offset atIndex:slot];
        else
            [encoder setBuffer:twinOf(object) offset:binding.offset atIndex:slot];
    }

    // The stride forms (setVertexBuffer:offset:attributeStride:atIndex: and the compute encoder's), whose stride a layout of
    // MTLBufferLayoutStrideDynamic reads
    void stridedBuffer(uint32_t stage, uint32_t slot, const raw::BufferBinding &binding)
    {
        const NSUInteger stride = binding.stride;
        id<MTLRenderCommandEncoder> render = encoder;
        id<MTLComputeCommandEncoder> compute = encoder;
        if (binding.source == raw::BufferBinding::Source::Offset) {
            if (stage == 1) {
                offsets[stage][slot] = binding.offset;
                [render setVertexBufferOffset:binding.offset attributeStride:stride atIndex:slot];
            } else
                [compute setBufferOffset:binding.offset attributeStride:stride atIndex:slot];
            return;
        }
        if (binding.source == raw::BufferBinding::Source::Bytes) {
            const void *bytes = cb->_bytes.chunks[binding.chunk].cpu + binding.offset;
            if (stage == 1) {
                bound[stage][slot] = nil;
                [render setVertexBytes:bytes length:binding.length attributeStride:stride atIndex:slot];
            } else {
                [compute setBytes:bytes length:binding.length attributeStride:stride atIndex:slot];
            }
            return;
        }
        id object = (__bridge id)binding.object;
        RMBuffer *buffer = [object isKindOfClass:[RMBuffer class]] ? object : nil;
        if (stage == 1) {
            bound[stage][slot] = buffer;
            offsets[stage][slot] = binding.offset;
            [render setVertexBuffer:twinOf(object) offset:binding.offset attributeStride:stride atIndex:slot];
        } else {
            if (buffer)
                buffers.insert(buffer);
            [compute setBuffer:twinOf(object) offset:binding.offset attributeStride:stride atIndex:slot];
        }
    }

    void texture(uint32_t stage, uint32_t slot, void *object)
    {
        id twin = twinOf((__bridge id)object);
        if (stage == 1)
            [encoder setVertexTexture:twin atIndex:slot];
        else if (stage == 2)
            [encoder setFragmentTexture:twin atIndex:slot];
        else
            [(id<MTLComputeCommandEncoder>)encoder setTexture:twin atIndex:slot];
    }

    void sampler(uint32_t stage, uint32_t slot, void *object)
    {
        id twin = twinOf((__bridge id)object);
        if (stage == 1)
            [encoder setVertexSamplerState:twin atIndex:slot];
        else if (stage == 2)
            [encoder setFragmentSamplerState:twin atIndex:slot];
        else
            [(id<MTLComputeCommandEncoder>)encoder setSamplerState:twin atIndex:slot];
    }

    void kernel(const raw::KernelState &next, const raw::KernelState *previous)
    {
        if (next.pipelineSet && (!previous || !previous->pipelineSet || previous->pipeline != next.pipeline))
            [encoder setComputePipelineState:twinOf((__bridge id)next.pipeline)];
        for (uint32_t i = 0; i < next.memory.size(); ++i)
            if ((next.memorySet & (1u << i)) &&
                (!previous || !(previous->memorySet & (1u << i)) || previous->memory[i] != next.memory[i]))
                [encoder setThreadgroupMemoryLength:next.memory[i] atIndex:i];
    }

    void raster(const raw::RasterState &next, const raw::RasterState *previous)
    {
        using R = raw::RasterState;
        using raw::rasterChanged;
        if (rasterChanged(next, previous, R::Pipeline, &R::pipeline)) {
            id object = (__bridge id)next.pipeline;
            pipeline = [object isKindOfClass:[RMPipeline class]] ? object : nil;
            [encoder setRenderPipelineState:twinOf(object)];
        }
        if (rasterChanged(next, previous, R::DepthStencil, &R::depthStencil))
            [encoder setDepthStencilState:twinOf((__bridge id)next.depthStencil)];
        if (rasterChanged(next, previous, R::Scissor, &R::scissor) ||
            rasterChanged(next, previous, R::Scissor, &R::scissors) ||
            rasterChanged(next, previous, R::Scissor, &R::scissorsAt)) {
            MTLScissorRect rects[raw::kMaxViewports];
            const uint32_t count = std::clamp<uint32_t>(next.scissors, 1, raw::kMaxViewports);
            rects[0] = (MTLScissorRect){next.scissor[0], next.scissor[1], next.scissor[2], next.scissor[3]};
            for (uint32_t j = 1; j < count; j++) {
                const uint64_t *at = cb->_plan.viewArrays.data() + next.scissorsAt + 4 * j;
                rects[j] = (MTLScissorRect){at[0], at[1], at[2], at[3]};
            }
            [encoder setScissorRects:rects count:count];
        }
        if (rasterChanged(next, previous, R::Viewport, &R::viewport) ||
            rasterChanged(next, previous, R::Viewport, &R::viewports) ||
            rasterChanged(next, previous, R::Viewport, &R::viewportsAt)) {
            MTLViewport viewports[raw::kMaxViewports];
            const uint32_t count = std::clamp<uint32_t>(next.viewports, 1, raw::kMaxViewports);
            for (uint32_t j = 0; j < count; j++) {
                const uint64_t *v = j ? cb->_plan.viewArrays.data() + next.viewportsAt + 6 * j : next.viewport.data();
                viewports[j] = (MTLViewport){raw::stateDouble(v[0]), raw::stateDouble(v[1]), raw::stateDouble(v[2]),
                                             raw::stateDouble(v[3]), raw::stateDouble(v[4]), raw::stateDouble(v[5])};
            }
            [encoder setViewports:viewports count:count];
        }
        if (rasterChanged(next, previous, R::Cull, &R::cull))
            [encoder setCullMode:MTLCullMode(next.cull)];
        if (rasterChanged(next, previous, R::Winding, &R::winding))
            [encoder setFrontFacingWinding:MTLWinding(next.winding)];
        if (rasterChanged(next, previous, R::Fill, &R::fill))
            [encoder setTriangleFillMode:MTLTriangleFillMode(next.fill)];
        if (rasterChanged(next, previous, R::DepthClip, &R::depthClip))
            [encoder setDepthClipMode:MTLDepthClipMode(next.depthClip)];
        if (rasterChanged(next, previous, R::Blend, &R::blend))
            [encoder setBlendColorRed:raw::stateFloat(next.blend[0])
                                green:raw::stateFloat(next.blend[1])
                                 blue:raw::stateFloat(next.blend[2])
                                alpha:raw::stateFloat(next.blend[3])];
        if (rasterChanged(next, previous, R::Stencil, &R::stencil))
            [encoder setStencilFrontReferenceValue:next.stencil[0] backReferenceValue:next.stencil[1]];
        if (rasterChanged(next, previous, R::DepthBias, &R::depthBias))
            [encoder setDepthBias:raw::stateFloat(next.depthBias[0])
                       slopeScale:raw::stateFloat(next.depthBias[1])
                            clamp:raw::stateFloat(next.depthBias[2])];
        if (rasterChanged(next, previous, R::Visibility, &R::visibility) ||
            rasterChanged(next, previous, R::Visibility, &R::visibilityOffset))
            [encoder setVisibilityResultMode:MTLVisibilityResultMode(next.visibility) offset:next.visibilityOffset];
        if (rasterChanged(next, previous, R::Amplification, &R::amplification)) {
            MTLVertexAmplificationViewMapping mappings[kAmplificationMappings];
            for (uint32_t k = 0; k < kAmplificationMappings; k++)
                mappings[k] = {next.amplification[1 + 2 * k], next.amplification[2 + 2 * k]};
            [encoder setVertexAmplificationCount:next.amplification[0] viewMappings:mappings];
        }
    }
};

}  // namespace

void replayPlannedState(RMCommandBuffer *cb, size_t at, id encoder, raw::StateCursor &cursor,
                        std::set<RMBuffer *> &buffers, RMBuffer *__strong bound[3][31], uint64_t offsets[3][31],
                        RMPipeline *__strong &pipeline)
{
    const Op op = cb->_plan.operation(at).op;
    ReplayPlanSink sink{cb, encoder, buffers, bound, offsets, pipeline};
    if (isPlanDraw(op))
        raw::applyRenderState(cb->_plan, cb->_plan.draw(at).state, cursor, sink);
    else if (op == OpDispatch)
        raw::applyKernelState(cb->_plan, cb->_plan.dispatch(at).state, cursor, sink);
    else if (op == OpInvocation || op == OpExecuteKernels) {
        const auto &control = cb->_plan.control(at);
        if (control.render != raw::kNoSnapshot)
            raw::applyRenderState(cb->_plan, control.render, cursor, sink);
        if (control.compute != raw::kNoSnapshot)
            raw::applyKernelState(cb->_plan, control.compute, cursor, sink);
    } else if (op == OpRender || op == OpCompute || op == OpEnd) {
        cursor.reset();
    }
}
