// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalReach.h"
#import "TegraGM20BMetalIndirect.h"
#import "TegraGM20BMetalPassErrors.h"
#import "TegraGM20BMetalPlanAdapters.h"

bool PlanDraws::any() const
{
    const SwitchMetalCommandPlan &plan = cb_->_plan;
    for (size_t at = from_; at < to_ && plan.operation(at).op != OpEnd; at++)
        if (isPlanDraw(plan.operation(at).op))
            return true;
    return false;
}

// An execute's draw names the topology of its buffer's triangles where it holds any (TegraGM20BMetalIndirect.mm execute:), so
// points and lines beside them are asked of the buffer
bool PlanDraws::atFrameResolution() const
{
    const SwitchMetalCommandPlan &plan = cb_->_plan;
    for (size_t at = from_; at < to_ && plan.operation(at).op != OpEnd; at++) {
        const Op op = plan.operation(at).op;
        if (!isPlanDraw(op))
            continue;
        const SwitchMetalPlanDraw &draw = plan.draw(at);
        const gm20b::RasterState &raster = plan.raster[plan.renderStates[draw.state].raster];
        if (gm20b::reachedAtFrameResolution(draw.primitive, raster.set & gm20b::RasterState::Fill ? raster.fill : 0))
            return true;
        if (op == OpExecuteCommands &&
            ([(__bridge SwitchMetalIndirectCommandBuffer *)draw.arguments kinds] & SwitchMetalICBOthers))
            return true;
    }
    return false;
}

// Each draw as the range encode draws it (TegraGM20BMetalRangeStep.mm), under its state's reach twin: an execute's commands inherit
// it, so the GPU draws them again by reference, and a patch draw runs the state's tessellation stages again
bool PlanDraws::replay(gm20b::RenderEncoder &e, const char **why)
{
    gm20b::StateCursor cursor;
    for (size_t at = from_; at < to_ && cb_->_plan.operation(at).op != OpEnd; at++) {
        const Op op = cb_->_plan.operation(at).op;
        if (!isPlanDraw(op))
            continue;
        encodePlannedState(cb_, at, commands_, &e, nullptr, cursor);
        e.takeReachTwin();
        if (e.reachCovers()) {
            e.setCullMode(0);
            if (!e.draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return false;
            cursor.reset();  // the next draw's state is applied whole, its culling with it
            continue;
        }
        const SwitchMetalPlanDraw &draw = cb_->_plan.draw(at);
        const uint32_t indexSize = draw.indexType == MTLIndexTypeUInt16 ? 2 : 4;
        SwitchMetalBuffer *indices = (__bridge SwitchMetalBuffer *)draw.indices,
                          *arguments = (__bridge SwitchMetalBuffer *)draw.arguments;
        bool drawn = false;
        switch (op) {
        case OpDraw:
            drawn = e.draw(draw.primitive, draw.first, draw.count, draw.instances, draw.baseInstance, why);
            break;
        case OpDrawIndexed:
            useBuffer(&commands_, indices, false);
            drawn = e.drawIndexed(draw.primitive, draw.count, indexSize, &indices->_raw, draw.indexOffset,
                                  draw.instances, draw.baseVertex, draw.baseInstance, why);
            break;
        case OpDrawIndirect:
            useBuffer(&commands_, arguments, false);
            drawn = e.drawIndirect(draw.primitive, &arguments->_raw, draw.argumentOffset, why);
            break;
        case OpDrawIndexedIndirect:
            useBuffer(&commands_, arguments, false);
            useBuffer(&commands_, indices, false);
            drawn = e.drawIndexedIndirect(draw.primitive, indexSize, &indices->_raw, draw.indexOffset, &arguments->_raw,
                                          draw.argumentOffset, why);
            break;
        case OpExecuteCommands:
            drawn = encodeCommands(&e, &commands_, draw, why);
            break;
        default:  // the patch draws, isPlanDraw's last
            drawn = encodePatches(&e, &commands_, draw, cb_->_plan.patches(at), op == OpDrawPatchesIndirect, why);
            break;
        }
        if (!drawn)
            return false;
    }
    return true;
}

void gm20bReachFails(SwitchMetalCommandBuffer *cb, bool ahead, const char *why, bool memory,
                     const gm20b::Refusal &refusal)
{
    if (memory)
        gm20bPassWorkFails(cb, ahead, why, MTLCommandBufferErrorOutOfMemory);
    else
        gm20bOwnDrawFails(cb, ahead, why, refusal);
}
