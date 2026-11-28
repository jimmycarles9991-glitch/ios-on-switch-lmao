// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "pass_hazard.h"

// What the Metal pass at `begin` samples and names, from what its recording kept (the plan's range facts), into `uses`
// (pass_hazard.h); its targets are the encoder's. Unknown where the range was not cached, a draw's uses are not
// final, it waits for a fence, names a heap or another kind of object, or writes memory other than its attachments.
__attribute__((objc_externally_retained)) inline void planPassUses(SwitchMetalCommandBuffer *cb, size_t begin,
                                                                   gm20b::PassUses &uses, bool own = false)
{
    const auto &header = own ? cb->_plan.ownBegin(begin) : cb->_plan.begin(begin);
    const size_t end = header.end;
    if (header.hasWaitFence)
        uses.known = false;
    if (header.unfinalDraws) {
        uses.known = false;
        uses.shaderWrites = true;
        uses.bufferWrites = true;
    }
    const SwitchMetalPass &pass = own ? cb->_passes.own(header.pass) : cb->_passes[header.pass];
    const uint8_t colorMask = pass.colorMask ? pass.colorMask : 0xffu;
    const auto attachment = [&](void *object) {
        for (uint8_t m = colorMask; m; m &= uint8_t(m - 1)) {
            const SwitchMetalPass::Color &a = pass.color[__builtin_ctz(m)];
            if ((__bridge void *)a.texture == object || (__bridge void *)a.resolve == object)
                return true;
        }
        return (__bridge void *)pass.depth == object || (__bridge void *)pass.depthResolve == object ||
               (__bridge void *)pass.stencilResolve == object;
    };
    const bool cached = cb->_plan.forEachRangeResource(begin, end, [&](const SwitchMetalRangeResource &use) {
        constexpr uint8_t reads = gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample | gm20b::ResourceUse::Explicit;
        switch (use.facts.kind) {
        case SwitchMetalResourceKind::Sampler:
            return;
        case SwitchMetalResourceKind::Buffer:
            if (use.access & gm20b::ResourceUse::Write) {
                uses.known = false;
                uses.shaderWrites = true;
                uses.bufferWrites = true;
            }
            return;
        case SwitchMetalResourceKind::Texture:
            break;
        default:
            uses.known = false;
            uses.shaderWrites = true;
            uses.bufferWrites = true;
            return;
        }
        const bool target = attachment(use.object);
        if ((use.access & gm20b::ResourceUse::Write) && !target) {
            uses.known = false;
            uses.shaderWrites = true;
        }
        if (target && !(use.access & (gm20b::ResourceUse::Sample | gm20b::ResourceUse::Explicit)))
            return;
        if (use.access & reads) {
            __unsafe_unretained SwitchMetalTexture *texture = (__bridge SwitchMetalTexture *)use.object;
            uses.sample(gm20b::hazardImage(texture->_raw), use.access & gm20b::ResourceUse::Explicit);
        }
    });
    if (!cached) {
        uses.known = false;
        uses.shaderWrites = true;
        uses.bufferWrites = true;
    }
}
