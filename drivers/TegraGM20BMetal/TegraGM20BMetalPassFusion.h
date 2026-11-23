// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "pass_fusion.h"
#include "attachment_lifetime.h"

// The pass's attachments and resources from what its recording kept (SwitchMetalPass, the plan's range facts): no message to
// its descriptor or its objects, and nothing retained. `shadows`: the command buffer binds a texture sampled through a pool
// image or a stand-in (as of the encode)
__attribute__((objc_externally_retained)) inline gm20b::FusionPass fusionPass(SwitchMetalCommandBuffer *cb,
                                                                              size_t begin, bool shadows)
{
    SWITCH_METAL_ENCODE_SPAN(Fusion);
    const Cmd command = cb->_plan.legacy(begin);
    const SwitchMetalPass &pass = cb->_passes[command.x];
    gm20b::FusionPass result;
    result.width = pass.width;
    result.height = pass.height;
    result.layers = pass.layers;
    // a fused pass's draws take the first pass's sample layout; each Metal pass's hidden depth starts at 1.0
    result.bodySafe = !shadows && !pass.positions.count && (pass.depth || !pass.testsHiddenZeta);
    std::array<__unsafe_unretained SwitchMetalTexture *, 10> targets{};
    auto attachment = [&](size_t slot, __unsafe_unretained id object, NSUInteger level, NSUInteger slice,
                          MTLLoadAction load, MTLStoreAction store, bool resolve) {
        if (!object)
            return;
        __unsafe_unretained SwitchMetalTexture *texture = (SwitchMetalTexture *)object;
        const auto &t = texture->_raw;
        targets[slot] = texture;
        auto &a = result.attachments[slot];
        a.identity = uintptr_t((__bridge void *)texture);
        a.level = level;
        a.slice = slice;
        a.loads = load == MTLLoadActionLoad;
        a.stores = store == MTLStoreActionStore;
        a.resolves = resolve;
        a.native = gm20b::fusionTargetEligible(t, level | slice);
    };
    for (size_t i = 0; i < 8; ++i) {
        const auto &a = pass.color[i];
        attachment(i, a.texture, a.level, a.slice, a.load, a.store, a.resolve != nil);
    }
    for (size_t i = 0; i < 2; ++i) {
        const auto &z = pass.zeta[i];
        attachment(8 + i, z.texture, z.level, z.slice, z.load, z.store, z.resolve);
    }
    const size_t end = cb->_plan.encoderEnd(begin);
    for (size_t i = begin + 1; i + 1 < end; ++i) {
        const Op op = cb->_plan.operation(i).op;
        if (!isPlanDraw(op) && op != OpBarrier)
            result.bodySafe = false;
        if (isPlanDraw(op) && !cb->_plan.renderStates[cb->_plan.draw(i).state].usesFinal)
            result.bodySafe = false;
    }
    if (!result.bodySafe)
        return result;
    const auto overlapsTarget = [&](__unsafe_unretained SwitchMetalTexture *texture, uint64_t va, uint64_t cpu,
                                    uint64_t bytes, const gm20b::Import *import) {
        for (__unsafe_unretained SwitchMetalTexture *attachment : targets) {
            if (!attachment)
                continue;
            const auto &t = attachment->_raw;
            const uint64_t size = gm20b::fusionImageBytes(t);
            if (texture == attachment || (import && import == t.import) ||
                gm20b::fusionRangesOverlap(va, bytes, t.va, size) ||
                gm20b::fusionRangesOverlap(cpu, bytes, uintptr_t(t.cpu), size) ||
                (import && t.import &&
                 gm20b::fusionRangesOverlap(import->base, import->bytes, t.import->base, t.import->bytes)))
                return true;
        }
        return false;
    };
    const bool cached = cb->_plan.forEachRangeResource(begin, end, [&](const SwitchMetalRangeResource &use) {
        const bool isTexture = use.facts.kind == SwitchMetalResourceKind::Texture,
                   isBuffer = use.facts.kind == SwitchMetalResourceKind::Buffer;
        __unsafe_unretained SwitchMetalTexture *texture = isTexture ? (__bridge SwitchMetalTexture *)use.object : nil;
        __unsafe_unretained SwitchMetalBuffer *buffer = isBuffer ? (__bridge SwitchMetalBuffer *)use.object : nil;
        const bool target = texture && std::find(targets.begin(), targets.end(), texture) != targets.end();
        if ((use.access & gm20b::ResourceUse::Write) && !target)
            result.bodySafe = false;
        if (target && !(use.access & (gm20b::ResourceUse::Sample | gm20b::ResourceUse::Explicit)))
            return;
        if (!texture && !buffer)
            return;
        const uint64_t va = texture ? texture->_raw.va : buffer->_raw.va;
        const uint64_t cpu = uintptr_t(texture ? texture->_raw.cpu : buffer->_raw.cpu);
        const uint64_t bytes = texture ? gm20b::fusionImageBytes(texture->_raw) : buffer->_raw.length;
        const gm20b::Import *import = texture ? texture->_raw.import : buffer->_raw.import;
        if (overlapsTarget(texture, va, cpu, bytes, import))
            result.bodySafe = false;
        // a converted texture is sampled through an image drawn from its planes: a target over a plane writes what the
        // image is drawn again from between the passes (TegraGM20BMetalRangeConversions.mm remakeImages), which fusion removes
        if (texture && texture->_raw.converted())
            for (const gm20b::Texture *plane : {texture->_raw.luma, texture->_raw.chroma})
                if (plane && overlapsTarget(nil, plane->va, uintptr_t(plane->cpu), gm20b::fusionImageBytes(*plane),
                                            plane->import))
                    result.bodySafe = false;
    });
    if (!cached)
        result.bodySafe = false;
    return result;
}
