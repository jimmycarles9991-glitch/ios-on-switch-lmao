// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalPlanAdapters.h"
#import "TegraGM20BMetalSplitPlan.h"
#include "compute.h"

namespace {

// The plan's bindings onto the encoders. The range's resources were named to the job before its encode
// (encodeRawAttempt) and every one a draw reads is named again where its binding table is built, so a binding names
// none; the command buffer holds every object, so none is retained here.
struct NativePlanSink {
    __unsafe_unretained SwitchMetalCommandBuffer *cb;
    gm20b::CommandBuffer &commands;
    gm20b::RenderEncoder *render;
    gm20b::ComputeEncoder *compute;
    const gm20b::Buffer *bytesChunks;
    const uint8_t *shadowData;
    const uint64_t *viewArrays;
    uint32_t *namedBytesChunk;

    void buffer(uint32_t stage, uint32_t slot, const gm20b::BufferBinding &binding)
    {
        SWITCH_METAL_ENCODE_SPAN(Apply);
        SWITCH_METAL_ENCODE_COUNT(Applied);
        if (stage == 3)
            compute->setBufferStride(slot, binding.stride);
        else if (stage == 1)
            render->setVertexStride(slot, binding.stride);
        if (binding.source == gm20b::BufferBinding::Source::Offset) {
            if (stage == 3)
                compute->setBufferOffset(slot, binding.offset);
            else
                render->setBufferOffset(stage, slot, binding.offset);
            return;
        }
        if (binding.source == gm20b::BufferBinding::Source::ClockAnchors) {
            compute->setKernelBytes(slot, binding.offset, binding.length);
            return;
        }
        if (binding.source == gm20b::BufferBinding::Source::Bytes) {
            SWITCH_METAL_ENCODE_SPAN(Uniforms);
            const gm20b::Buffer &chunk = bytesChunks[binding.chunk];
            if (!chunk.wc)
                commands.clean(chunk.cpu + binding.offset, std::max<uint32_t>((binding.length + 255) & ~255u, 256));
            if (stage == 3) {
                compute->setBytesAt(slot, chunk.va + binding.offset, binding.length, &chunk);
            } else {
                const uint8_t *cpu = binding.shadow
                                         ? shadowData + binding.shadow - 1
                                         : (stage == 1 && chunk.wc ? nullptr : chunk.cpu + binding.offset);
                if (!namedBytesChunk || *namedBytesChunk != binding.chunk) {
                    commands.use(chunk, false);
                    if (namedBytesChunk)
                        *namedBytesChunk = binding.chunk;
                }
                render->setNamedBytesAt(int(stage), slot, chunk.va + binding.offset, binding.length, cpu, chunk.va);
            }
            return;
        }
        __unsafe_unretained SwitchMetalBuffer *object = (__bridge SwitchMetalBuffer *)binding.object;
        if (stage == 3)
            compute->setBuffer(slot, object ? &object->_raw : nullptr, binding.offset);
        else
            render->setBuffer(stage, slot, object ? &object->_raw : nullptr, binding.offset);
    }

    void texture(uint32_t stage, uint32_t slot, void *value)
    {
        SWITCH_METAL_ENCODE_SPAN(Apply);
        SWITCH_METAL_ENCODE_COUNT(Applied);
        __unsafe_unretained SwitchMetalTexture *object = (__bridge SwitchMetalTexture *)value;
        if (stage == 3)
            compute->setTexture(slot, object ? &object->_raw : nullptr);
        else
            render->setTexture(int(stage), slot, object ? &object->_raw : nullptr);
    }

    void sampler(uint32_t stage, uint32_t slot, void *value)
    {
        SWITCH_METAL_ENCODE_SPAN(Apply);
        SWITCH_METAL_ENCODE_COUNT(Applied);
        __unsafe_unretained SwitchMetalSampler *object = (__bridge SwitchMetalSampler *)value;
        if (stage == 3)
            compute->setSampler(slot, object ? &object->_raw : nullptr);
        else
            render->setSampler(int(stage), slot, object ? &object->_raw : nullptr);
    }

    void kernel(const gm20b::KernelState &, const gm20b::KernelState *)
    {}

    void raster(const gm20b::RasterState &next, const gm20b::RasterState *previous)
    {
        SWITCH_METAL_ENCODE_SPAN(Apply);
        SWITCH_METAL_ENCODE_COUNT(Applied);
        using R = gm20b::RasterState;
        using gm20b::rasterChanged;
        if (rasterChanged(next, previous, R::Pipeline, &R::pipeline) ||
            rasterChanged(next, previous, R::Pipeline, &R::nativePipeline) ||
            rasterChanged(next, previous, R::Pipeline, &R::nativeVariant)) {
            __unsafe_unretained SwitchMetalPipeline *pipeline = (__bridge SwitchMetalPipeline *)next.pipeline;
            render->setPipeline(nativeOf(next));
            render->setAlphaControl(pipeline ? pipeline->_alphaControl : 0);
        }
        if (rasterChanged(next, previous, R::Scissor, &R::scissor) ||
            rasterChanged(next, previous, R::Scissor, &R::scissors) ||
            rasterChanged(next, previous, R::Scissor, &R::scissorsAt)) {
            if (next.scissors > 1) {
                uint32_t rects[gm20b::kMaxViewports][4];
                const uint32_t count = std::min<uint32_t>(next.scissors, gm20b::kMaxViewports);
                for (uint32_t j = 0; j < count; j++)
                    for (uint32_t i = 0; i < 4; i++)
                        rects[j][i] = uint32_t(viewArrays[next.scissorsAt + 4 * j + i]);
                render->setScissors(rects, count);
            } else {
                render->setScissor(next.scissor[0], next.scissor[1], next.scissor[2], next.scissor[3]);
            }
        }
        if (!(next.set & ~(R::Pipeline | R::Scissor)))
            return;
        if (rasterChanged(next, previous, R::DepthStencil, &R::depthStencil)) {
            __unsafe_unretained SwitchMetalDepthStencil *depth = (__bridge SwitchMetalDepthStencil *)next.depthStencil;
            render->setDepthStencil(depth ? &depth->_raw : nullptr);
        }
        if (rasterChanged(next, previous, R::Viewport, &R::viewport) ||
            rasterChanged(next, previous, R::Viewport, &R::viewports) ||
            rasterChanged(next, previous, R::Viewport, &R::viewportsAt)) {
            if (next.viewports > 1) {
                double viewports[gm20b::kMaxViewports][6];
                const uint32_t count = std::min<uint32_t>(next.viewports, gm20b::kMaxViewports);
                for (uint32_t j = 0; j < count; j++)
                    for (uint32_t i = 0; i < 6; i++)
                        viewports[j][i] = gm20b::stateDouble(viewArrays[next.viewportsAt + 6 * j + i]);
                render->setViewports(viewports, count);
            } else {
                render->setViewport(gm20b::stateDouble(next.viewport[0]), gm20b::stateDouble(next.viewport[1]),
                                    gm20b::stateDouble(next.viewport[2]), gm20b::stateDouble(next.viewport[3]),
                                    gm20b::stateDouble(next.viewport[4]), gm20b::stateDouble(next.viewport[5]));
            }
        }
        if (rasterChanged(next, previous, R::Cull, &R::cull))
            render->setCullMode(uint8_t(next.cull));
        if (rasterChanged(next, previous, R::Winding, &R::winding))
            render->setWinding(uint8_t(next.winding));
        if (rasterChanged(next, previous, R::Fill, &R::fill))
            render->setFillMode(uint8_t(next.fill));
        if (rasterChanged(next, previous, R::DepthClip, &R::depthClip))
            render->setDepthClamp(next.depthClip == MTLDepthClipModeClamp);
        if (rasterChanged(next, previous, R::Blend, &R::blend)) {
            const float values[4] = {gm20b::stateFloat(next.blend[0]), gm20b::stateFloat(next.blend[1]),
                                     gm20b::stateFloat(next.blend[2]), gm20b::stateFloat(next.blend[3])};
            render->setBlendColor(values);
        }
        if (rasterChanged(next, previous, R::Stencil, &R::stencil))
            render->setStencilReference(next.stencil[0], next.stencil[1]);
        if (rasterChanged(next, previous, R::DepthBias, &R::depthBias))
            render->setDepthBias(gm20b::stateFloat(next.depthBias[0]), gm20b::stateFloat(next.depthBias[1]),
                                 gm20b::stateFloat(next.depthBias[2]));
        if (rasterChanged(next, previous, R::Amplification, &R::amplification)) {
            uint32_t viewports[kAmplificationMappings], layers[kAmplificationMappings];
            for (uint32_t k = 0; k < kAmplificationMappings; k++) {
                viewports[k] = next.amplification[1 + 2 * k];
                layers[k] = next.amplification[2 + 2 * k];
            }
            render->setAmplification(next.amplification[0], viewports, layers);
        }
    }
};

}  // namespace

// (the command buffer is its caller's, so ARC need not retain it at each command)
__attribute__((objc_externally_retained)) void
encodePlannedDrawState(SwitchMetalCommandBuffer *cb, uint32_t stateIndex, gm20b::CommandBuffer &commands,
                       gm20b::RenderEncoder *render, gm20b::StateCursor &cursor, bool own, uint32_t *namedBytesChunk)
{
    if (cursor.render == stateIndex)
        return;
    NativePlanSink sink{cb,
                        commands,
                        render,
                        nullptr,
                        own ? cb->_bytes.chunks.ownData() : cb->_bytes.chunks.data(),
                        own ? cb->_shadow.ownData() : cb->_shadow.data(),
                        own ? cb->_plan.viewArrays.ownData() : cb->_plan.viewArrays.data(),
                        namedBytesChunk};
    gm20b::applyRenderState(cb->_plan, stateIndex, cursor, sink, own);
}

__attribute__((objc_externally_retained)) void
encodePlannedState(SwitchMetalCommandBuffer *cb, size_t at, gm20b::CommandBuffer &commands,
                   gm20b::RenderEncoder *render, gm20b::ComputeEncoder *compute, gm20b::StateCursor &cursor, bool own,
                   uint32_t *namedBytesChunk)
{
    const auto &operation = own ? cb->_plan.ownOperation(at) : cb->_plan.operation(at);
    const Op op = operation.op;
    if (isPlanDraw(op)) {
        encodePlannedDrawState(cb, cb->_plan.drawAt(operation.payload, own).state, commands, render, cursor, own,
                               namedBytesChunk);
        return;
    }
    if (op == OpRender || op == OpCompute || op == OpEnd) {
        cursor.reset();
        return;
    }
    NativePlanSink sink{cb,
                        commands,
                        render,
                        compute,
                        own ? cb->_bytes.chunks.ownData() : cb->_bytes.chunks.data(),
                        own ? cb->_shadow.ownData() : cb->_shadow.data(),
                        own ? cb->_plan.viewArrays.ownData() : cb->_plan.viewArrays.data(),
                        namedBytesChunk};
    if (op == OpDispatch)
        gm20b::applyKernelState(cb->_plan, cb->_plan.dispatch(at).state, cursor, sink, own);
    else if (op == OpExecuteKernels && cb->_plan.control(at).compute != gm20b::kNoSnapshot)
        gm20b::applyKernelState(cb->_plan, cb->_plan.control(at).compute, cursor, sink, own);
}
