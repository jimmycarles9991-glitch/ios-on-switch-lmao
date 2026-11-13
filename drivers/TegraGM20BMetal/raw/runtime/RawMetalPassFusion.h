// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_pass_fusion.h"
#include "raw_attachment_lifetime.h"

// The pass's attachments and resources from what its recording kept (RMPass, the plan's range facts): no message to
// its descriptor or its objects, and nothing retained
__attribute__((objc_externally_retained)) inline raw::FusionPass fusionPass(RMCommandBuffer *cb, size_t begin)
{
    RAW_ENCODE_SPAN(Fusion);
    const Cmd command = cb->_plan.legacy(begin);
    const RMPass &pass = cb->_passes[command.x];
    raw::FusionPass result;
    result.width = pass.width;
    result.height = pass.height;
    result.layers = pass.layers;
    // a fused pass's draws take the first pass's sample layout; each Metal pass's hidden depth starts at 1.0
    result.bodySafe =
        !cb->_sampledShadows && !cb->_standIns && !pass.positions.count && (pass.depth || !pass.testsHiddenZeta);
    std::array<__unsafe_unretained RMTexture *, 10> targets{};
    auto attachment = [&](size_t slot, __unsafe_unretained id object, NSUInteger level, NSUInteger slice,
                          MTLLoadAction load, MTLStoreAction store, bool resolve) {
        if (!object)
            return;
        __unsafe_unretained RMTexture *texture = (RMTexture *)object;
        const auto &t = texture->_raw;
        targets[slot] = texture;
        auto &a = result.attachments[slot];
        a.identity = uintptr_t((__bridge void *)texture);
        a.level = level;
        a.slice = slice;
        a.loads = load == MTLLoadActionLoad;
        a.stores = store == MTLStoreActionStore;
        a.resolves = resolve;
        a.native = raw::fusionTargetEligible(t, level | slice);
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
    const bool cached = cb->_plan.forEachRangeResource(begin, end, [&](const RMRangeResource &use) {
        const bool isTexture = use.facts.kind == RMResourceKind::Texture,
                   isBuffer = use.facts.kind == RMResourceKind::Buffer;
        __unsafe_unretained RMTexture *texture = isTexture ? (__bridge RMTexture *)use.object : nil;
        __unsafe_unretained RMBuffer *buffer = isBuffer ? (__bridge RMBuffer *)use.object : nil;
        const bool target = texture && std::find(targets.begin(), targets.end(), texture) != targets.end();
        if ((use.access & raw::ResourceUse::Write) && !target)
            result.bodySafe = false;
        if (target && !(use.access & (raw::ResourceUse::Sample | raw::ResourceUse::Explicit)))
            return;
        if (!texture && !buffer)
            return;
        const uint64_t va = texture ? texture->_raw.va : buffer->_raw.va;
        const uint64_t cpu = uintptr_t(texture ? texture->_raw.cpu : buffer->_raw.cpu);
        const uint64_t bytes = texture ? raw::fusionImageBytes(texture->_raw) : buffer->_raw.length;
        const raw::Import *import = texture ? texture->_raw.import : buffer->_raw.import;
        for (__unsafe_unretained RMTexture *attachment : targets) {
            if (!attachment)
                continue;
            const auto &t = attachment->_raw;
            const uint64_t size = raw::fusionImageBytes(t);
            if (texture == attachment || (import && import == t.import) ||
                raw::fusionRangesOverlap(va, bytes, t.va, size) ||
                raw::fusionRangesOverlap(cpu, bytes, uintptr_t(t.cpu), size) ||
                (import && t.import &&
                 raw::fusionRangesOverlap(import->base, import->bytes, t.import->base, t.import->bytes)))
                result.bodySafe = false;
        }
    });
    if (!cached)
        result.bodySafe = false;
    return result;
}
