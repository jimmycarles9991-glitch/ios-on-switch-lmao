// SPDX-License-Identifier: MIT
// What m2v has no form of: its frontend's encoders implement no drawPatches:, setTessellationFactorBuffer:,
// setVertexAmplificationCount:viewMappings: or attribute stride forms (NativeBridgeMetalFrontend.mm answers each zero,
// so m2v draws without them) and it makes no indirect command buffer; a state the raw path made of tessellation,
// amplification, a dynamic attribute stride or stage-in is drawn by raw alone; the replay of indirect draw commands
// reads no range the GPU wrote (replayCommands); m2v has no layered form of a state the raw path made for one layer only
// (raw_layers.h layeredDrawMissing). A range of a command buffer the raw path refused that holds any of these never
// replays there: the command buffer fails as Metal reports an error, naming the refusal.
#import "RawMetalIndirectCompute.h"
#include "raw_layers.h"

namespace {

const char *drawMissing(RMCommandBuffer *cb, size_t at, Op op, uint32_t layers)
{
    if (op == OpDrawPatches || op == OpDrawPatchesIndirect)
        return "a patch draw";
    const auto &plan = cb->_plan;
    if (op == OpExecuteCommands && ([(__bridge RMIndirectCommandBuffer *)plan.draw(at).arguments kinds] & RMICBPatches))
        return "indirect commands of patches";
    if (op == OpExecuteCommands && plan.draw(at).indices)
        return "an execute of indirect commands by a range the GPU reads";
    const raw::RenderStateRef &state = plan.renderStates[plan.draw(at).state];
    const raw::RasterState &raster = plan.raster[state.raster];
    if (raster.amplification[0] > 1)
        return "a vertex amplification count above 1";
    id pipeline = (__bridge id)raster.pipeline;
    if ([pipeline isKindOfClass:[RMPipeline class]] && ((RMPipeline *)pipeline)->_m2vMissing)
        return ((RMPipeline *)pipeline)->_m2vMissing;
    if (const raw::Pipeline *made =
            [pipeline isKindOfClass:[RMPipeline class]] ? ((RMPipeline *)pipeline)->_raw : nullptr)
        if (const char *missing = raw::layeredDrawMissing(made->rec->arrayLengthWord, layers, raw::kLayerDropped))
            return missing;
    const auto &vertexBuffers = plan.buffers[0][state.buffers[0]];
    for (uint32_t slot = 0; slot < 31; slot++)
        if (vertexBuffers[slot].stride != raw::kStaticStride)
            return "a vertex buffer bound with an attribute stride";
    return nullptr;
}

const char *kernelMissing(id state)
{
    return [state isKindOfClass:[RMComputeState class]] ? ((RMComputeState *)state)->_m2vMissing : nullptr;
}

// The compute encoder's state `index` (RMCommandPlan::kernelStates) at a dispatch or an execute of dispatch commands
const char *kernelStateMissing(RMCommandBuffer *cb, uint32_t index)
{
    const auto &plan = cb->_plan;
    const raw::KernelStateRef &state = plan.kernelStates[index];
    if (const char *missing = kernelMissing((__bridge id)plan.kernel[state.state].pipeline))
        return missing;
    const auto &buffers = plan.buffers[2][state.buffers];
    for (uint32_t slot = 0; slot < 31; slot++)
        if (buffers[slot].stride != raw::kStaticStride)
            return "a kernel buffer bound with an attribute stride";
    return nullptr;
}

// m2v's frontend makes no indirect command buffer (its device answers newIndirectCommandBufferWithDescriptor: nil), so
// replayKernelCommands runs none of the commands
const char *executeMissing(RMCommandBuffer *cb, size_t at)
{
    (void)cb, (void)at;
    return "an execute of indirect dispatch commands, of which m2v makes no buffer";
}

// What of [from, to) m2v has no form of (null: nothing)
const char *rangeMissing(RMCommandBuffer *cb, size_t from, size_t to)
{
    // the layers of the pass a draw of the range is in: the last OpRender at or before it
    uint32_t layers = 1;
    for (size_t at = from; at-- > 0;)
        if (cb->_plan.operation(at).op == OpRender) {
            layers = cb->_passes[cb->_plan.legacy(at).x].layers;
            break;
        }
    for (size_t at = from; at < to; at++) {
        const Op op = cb->_plan.operation(at).op;
        const char *missing = nullptr;
        if (op == OpRender)
            layers = cb->_passes[cb->_plan.legacy(at).x].layers;
        if (isPlanDraw(op))
            missing = drawMissing(cb, at, op, layers);
        else if (op == OpDispatch)
            missing = kernelStateMissing(cb, cb->_plan.dispatch(at).state);
        else if (op == OpExecuteKernels)
            missing = executeMissing(cb, at);
        if (missing)
            return missing;
    }
    return nullptr;
}

}  // namespace

const char *rmM2vMissingState(MTLRenderPipelineDescriptor *d)
{
    if (d.vertexFunction.patchType != MTLPatchTypeNone)
        return "a tessellation state";
    if (d.maxVertexAmplificationCount > 1)
        return "a state of vertex amplification";
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (NSUInteger i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format != MTLVertexFormatInvalid && a.bufferIndex < 31 &&
                v.layouts[a.bufferIndex].stride == MTLBufferLayoutStrideDynamic)
                return "a state of a dynamic attribute stride";
        }
    return nullptr;
}

const char *rmM2vMissingKernel(MTLComputePipelineDescriptor *d)
{
    MTLStageInputOutputDescriptor *in = d.stageInputDescriptor;
    for (NSUInteger i = 0; in && i < 31; i++)
        if (in.attributes[i].format != MTLAttributeFormatInvalid)
            return "a stage-in kernel";
    return nullptr;
}

bool rmRefuseM2vRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    const char *missing = rangeMissing(cb, from, to);
    if (!missing)
        return false;
    NSString *refusal = cb->_why ?: @"?";
    SWITCH_REFUSE_DETAIL("raw", "replay.m2v_missing", uintptr_t(missing),
                         "a command buffer range the raw path refused (%s) holds %s, which m2v has no form of: the "
                         "command buffer fails",
                         refusal.UTF8String, missing);
    failCommandBuffer(cb, [NSString stringWithFormat:@"The raw path refused %@, and the work it left to m2v holds %s, "
                                                     @"which m2v does not draw",
                                                     refusal, missing]);
    return true;
}
