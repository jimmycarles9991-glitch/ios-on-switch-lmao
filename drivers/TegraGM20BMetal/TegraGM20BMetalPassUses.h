// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "pass_hazard.h"

// What the Metal pass at `begin` samples and names, from what its recording kept (the plan's range facts), into `uses`
// (pass_hazard.h); its targets are the encoder's. Unknown where the range was not cached, a draw's uses are not
// final, it waits for a fence, names a heap or another kind of object, or writes memory other than its attachments.
__attribute__((objc_externally_retained)) inline void planPassUses(SwitchMetalCommandBuffer *cb, size_t begin,
                                                                   gm20b::PassUses &uses)
{
    const size_t end = cb->_plan.encoderEnd(begin);
    for (size_t i = begin + 1; i + 1 < end; ++i) {
        const Op op = cb->_plan.operation(i).op;
        if (op == OpWaitFence || (isPlanDraw(op) && !cb->_plan.renderStates[cb->_plan.draw(i).state].usesFinal))
            uses.known = false;
    }
    const SwitchMetalPass &pass = cb->_passes[cb->_plan.legacy(begin).x];
    const auto attachment = [&](void *object) {
        for (const SwitchMetalPass::Color &a : pass.color)
            if ((__bridge void *)a.texture == object || (__bridge void *)a.resolve == object)
                return true;
        return (__bridge void *)pass.depth == object || (__bridge void *)pass.depthResolve == object ||
               (__bridge void *)pass.stencilResolve == object;
    };
    const bool cached = cb->_plan.forEachRangeResource(begin, end, [&](const SwitchMetalRangeResource &use) {
        constexpr uint8_t reads = gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample | gm20b::ResourceUse::Explicit;
        switch (use.facts.kind) {
        case SwitchMetalResourceKind::Sampler:
            return;
        case SwitchMetalResourceKind::Buffer:
            if (use.access & gm20b::ResourceUse::Write)
                uses.known = false;
            return;
        case SwitchMetalResourceKind::Texture:
            break;
        default:
            uses.known = false;
            return;
        }
        const bool target = attachment(use.object);
        if ((use.access & gm20b::ResourceUse::Write) && !target)
            uses.known = false;
        if (target && !(use.access & (gm20b::ResourceUse::Sample | gm20b::ResourceUse::Explicit)))
            return;
        if (use.access & reads) {
            __unsafe_unretained SwitchMetalTexture *texture = (__bridge SwitchMetalTexture *)use.object;
            uses.sample(gm20b::hazardImage(texture->_raw), use.access & gm20b::ResourceUse::Explicit);
        }
    });
    if (!cached)
        uses.known = false;
}
