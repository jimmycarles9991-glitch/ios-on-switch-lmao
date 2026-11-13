// SPDX-License-Identifier: MIT
#include "plan_fusion.h"
#include "../runtime/raw_pass_fusion.h"

namespace raw::plan {

namespace {

bool nativeTarget(const ResourceFacts &f, uint16_t level)
{
    constexpr uint32_t drawnElsewhere = ResourceFacts::Memoryless | ResourceFacts::SampledShadow |
                                        ResourceFacts::ExtendedRange | ResourceFacts::PlanarStandIn |
                                        ResourceFacts::PlanarCodes | ResourceFacts::Ycbcr;
    return !(f.flags & drawnElsewhere) && !level;
}

bool sharesMemory(const Recording &r, uint32_t a, uint32_t b)
{
    if (a == b)
        return true;
    const ResourceFacts &x = r.resources[a], &y = r.resources[b];
    if (x.memory == kNone || x.memory != y.memory)
        return false;
    return !x.bytes || !y.bytes || (x.offset < y.offset + y.bytes && y.offset < x.offset + x.bytes);
}

// RawMetalPassFusion.h fusionPass: the pass's attachments, and whether its body is draws alone over memory no target
// shares, writing nothing but its targets
raw::FusionPass fusionPass(const Recording &r, const Encoder &e)
{
    const PassDesc &pass = r.passes[e.pass];
    raw::FusionPass out;
    out.width = pass.width;
    out.height = pass.height;
    out.bodySafe = true;
    std::array<uint32_t, 10> targets;
    targets.fill(kNone);
    const auto attachment = [&](size_t i, const Attachment &a) {
        if (a.texture == kNone)
            return;
        targets[i] = a.texture;
        auto &f = out.attachments[i];
        f.identity = uintptr_t(a.texture) + 1;
        f.level = a.level;
        f.loads = a.load == Load::Load;
        f.stores = a.store == Store::Store;
        f.resolves = a.resolve != kNone;
        f.native = nativeTarget(r.resources[a.texture], a.level);
    };
    for (size_t i = 0; i < 8; ++i)
        attachment(i, pass.color[i]);
    attachment(8, pass.depth);
    attachment(9, pass.stencil);
    for (uint32_t i = 0; i < e.ops.count && out.bodySafe; ++i) {
        const Op &op = r.ops[e.ops.first + i];
        if (op.kind == OpKind::Control && r.controls[op.payload].kind == Control::MemoryBarrier)
            continue;
        if (op.kind != OpKind::Draw) {
            out.bodySafe = false;
            break;
        }
        const Draw &d = r.draws[op.payload];
        const DrawState &s = r.states[d.state];
        for (uint32_t k = 0; k < s.bindings.count; ++k) {
            const Binding &b = r.bindings[s.bindings.first + k];
            if (b.resource == kNone)
                continue;
            for (uint32_t t : targets)
                if (t != kNone && sharesMemory(r, b.resource, t))
                    out.bodySafe = false;
            if (b.write)
                out.bodySafe = false;
        }
    }
    return out;
}

}  // namespace

bool fusible(const Recording &r, const Encoder &first, const Encoder &next)
{
    if (first.kind != EncoderKind::Render || next.kind != EncoderKind::Render)
        return false;
    return raw::canFusePasses(fusionPass(r, first), fusionPass(r, next));
}

}  // namespace raw::plan
