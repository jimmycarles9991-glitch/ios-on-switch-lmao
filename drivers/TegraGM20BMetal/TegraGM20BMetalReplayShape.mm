// SPDX-License-Identifier: MIT
// DEVELOPMENT: a raw range's structure (replay_shape.h) from its recorded plan, counted once its commit is accepted.
#import "TegraGM20BMetalInternal.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include "replay_shape.h"

namespace {

void mixPass(gm20b::ReplayShape &shape, const SwitchMetalPass &pass)
{
    for (const SwitchMetalPass::Color &c : pass.color) {
        SwitchMetalTexture *t = c.texture;
        shape.mix(t ? t->_raw.format : 0);
        shape.mix(uint64_t(c.load) << 8 | uint64_t(c.store));
    }
    SwitchMetalTexture *depth = pass.depth;
    shape.mix(depth ? depth->_raw.format : 0);
    shape.mix(uint64_t(pass.depthLoad) << 8 | uint64_t(pass.stencilLoad));
    shape.mix(gm20b::ReplayShape::sizeClass(pass.width) << 8 | gm20b::ReplayShape::sizeClass(pass.height));
}

template <typename Mask> void mixSlots(gm20b::ReplayShape &shape, const Mask &slots)
{
    for (size_t w = 0; w < gm20b::SlotMaskWords<Mask>::kWords; ++w)
        shape.mix(gm20b::SlotMaskWords<Mask>::word(slots, w));
}

void mixBindings(gm20b::ReplayShape &shape, const SwitchMetalCommandPlan &plan, const gm20b::RenderStateRef &state)
{
    for (uint32_t stage = 0; stage < 2; ++stage) {
        uint64_t kinds = 0;
        for (const gm20b::BufferBinding &b : plan.buffers[stage][state.buffers[stage]])
            kinds = kinds << 2 | uint64_t(b.source);
        shape.mix(kinds);
        mixSlots(shape, plan.textures[stage][state.textures[stage]].set);
        shape.mix(plan.samplers[stage][state.samplers[stage]].set);
    }
}

void mixDraw(gm20b::ReplayShape &shape, const SwitchMetalCommandPlan &plan, const SwitchMetalPlanDraw &draw)
{
    const gm20b::RenderStateRef &state = plan.renderStates[draw.state];
    const gm20b::RasterState &raster = plan.raster[state.raster];
    shape.mix(reinterpret_cast<uintptr_t>(raster.pipeline));
    shape.mix(reinterpret_cast<uintptr_t>(raster.depthStencil));
    shape.mix(uint64_t(draw.primitive) << 16 | uint64_t(draw.indexType) << 8 | (draw.instances > 1));
    shape.mix(gm20b::ReplayShape::sizeClass(draw.count));
    mixBindings(shape, plan, state);
}

}  // namespace

void noteReplayShape(SwitchMetalCommandBuffer *cb, size_t from, size_t to)
{
    gm20b::ReplayShape shape;
    const SwitchMetalCommandPlan &plan = cb->_plan;
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
