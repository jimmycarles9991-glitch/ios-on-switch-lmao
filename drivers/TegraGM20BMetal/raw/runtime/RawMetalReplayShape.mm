// SPDX-License-Identifier: MIT
// DEVELOPMENT: a raw range's structure (raw_replay_shape.h) from its recorded plan, counted once its commit is accepted.
#import "RawMetalInternal.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include "raw_replay_shape.h"

namespace {

void mixPass(raw::ReplayShape &shape, const RMPass &pass)
{
    for (const RMPass::Color &c : pass.color) {
        RMTexture *t = c.texture;
        shape.mix(t ? t->_raw.format : 0);
        shape.mix(uint64_t(c.load) << 8 | uint64_t(c.store));
    }
    RMTexture *depth = pass.depth;
    shape.mix(depth ? depth->_raw.format : 0);
    shape.mix(uint64_t(pass.depthLoad) << 8 | uint64_t(pass.stencilLoad));
    shape.mix(raw::ReplayShape::sizeClass(pass.width) << 8 | raw::ReplayShape::sizeClass(pass.height));
}

void mixBindings(raw::ReplayShape &shape, const RMCommandPlan &plan, const raw::RenderStateRef &state)
{
    for (uint32_t stage = 0; stage < 2; ++stage) {
        uint64_t kinds = 0;
        for (const raw::BufferBinding &b : plan.buffers[stage][state.buffers[stage]])
            kinds = kinds << 2 | uint64_t(b.source);
        shape.mix(kinds);
        shape.mix(plan.textures[stage][state.textures[stage]].set);
        shape.mix(plan.samplers[stage][state.samplers[stage]].set);
    }
}

void mixDraw(raw::ReplayShape &shape, const RMCommandPlan &plan, const RMPlanDraw &draw)
{
    const raw::RenderStateRef &state = plan.renderStates[draw.state];
    const raw::RasterState &raster = plan.raster[state.raster];
    shape.mix(reinterpret_cast<uintptr_t>(raster.pipeline));
    shape.mix(reinterpret_cast<uintptr_t>(raster.depthStencil));
    shape.mix(uint64_t(draw.primitive) << 16 | uint64_t(draw.indexType) << 8 | (draw.instances > 1));
    shape.mix(raw::ReplayShape::sizeClass(draw.count));
    mixBindings(shape, plan, state);
}

}  // namespace

void noteReplayShape(RMCommandBuffer *cb, size_t from, size_t to)
{
    raw::ReplayShape shape;
    const RMCommandPlan &plan = cb->_plan;
    for (size_t at = from; at < to; ++at) {
        const Cmd c = plan.legacy(at);
        shape.mix(c.op);
        if (c.op == OpRender)
            mixPass(shape, cb->_passes[c.x]);
        else if (isPlanDraw(c.op))
            mixDraw(shape, plan, plan.draw(at));
        else if (c.op == OpDispatch)
            shape.mix(reinterpret_cast<uintptr_t>(plan.dispatch(at).kernel));
    }
    gRM->raw->stats.replay.note(shape.value());
}
#endif
