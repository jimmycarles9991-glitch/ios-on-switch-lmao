// SPDX-License-Identifier: MIT
#import "RawMetalReach.h"
#import "RawMetalPlanAdapters.h"

bool PlanDraws::any() const
{
    const RMCommandPlan &plan = cb_->_plan;
    for (size_t at = from_; at < to_ && plan.operation(at).op != OpEnd; at++)
        if (isPlanDraw(plan.operation(at).op))
            return true;
    return false;
}

bool PlanDraws::atFrameResolution() const
{
    const RMCommandPlan &plan = cb_->_plan;
    for (size_t at = from_; at < to_ && plan.operation(at).op != OpEnd; at++) {
        if (!isPlanDraw(plan.operation(at).op))
            continue;
        const RMPlanDraw &draw = plan.draw(at);
        const raw::RasterState &raster = plan.raster[plan.renderStates[draw.state].raster];
        if (raw::reachedAtFrameResolution(draw.primitive, raster.set & raw::RasterState::Fill ? raster.fill : 0))
            return true;
    }
    return false;
}

bool PlanDraws::replay(raw::RenderEncoder &e, const char **why)
{
    raw::StateCursor cursor;
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
        const RMPlanDraw &draw = cb_->_plan.draw(at);
        const uint32_t indexSize = draw.indexType == MTLIndexTypeUInt16 ? 2 : 4;
        RMBuffer *indices = (__bridge RMBuffer *)draw.indices, *arguments = (__bridge RMBuffer *)draw.arguments;
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
        default:
            *why = "a turned pass's reach replays no indirect command buffer or patch draw";
            return false;
        }
        if (!drawn)
            return false;
    }
    return true;
}
