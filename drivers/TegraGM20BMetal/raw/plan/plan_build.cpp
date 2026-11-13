// SPDX-License-Identifier: MIT
#include "plan_build.h"
#include <map>
#include "plan_builder.h"
#include "plan_fusion.h"
#include "plan_key.h"
#include <algorithm>

namespace raw::plan {

namespace {

constexpr uint8_t kFragmentStage = 2;

Stage stageOf(uint8_t stage)
{
    return stage == 0 ? Stage::Vertex : stage == 1 ? Stage::Fragment : Stage::Compute;
}

// A layered pass writes renderTargetArrayLength layers from the attachment's slice (a 3D texture's from its depth plane)
SubRange levelOf(const Attachment &a, uint16_t layers)
{
    SubRange r;
    r.level = a.level;
    r.levels = 1;
    r.layer = a.depthPlane ? a.depthPlane : a.slice;
    r.layers = std::max<uint16_t>(layers, 1);
    return r;
}

// Metal's memoryBarrierWithScope:afterStages:beforeStages: inside a pass: colour targets between fragment stages are
// pixel-ordered; buffer and texture stores need the SMs' L1 flushed and the readers' caches dropped
Barriers memoryBarrier(const Control &c, bool tiledCache)
{
    Barriers b = 0;
    if (c.scope & Control::RenderTargets) {
        if (c.after == kFragmentStage && c.before == kFragmentStage)
            b |= PixelShaderBarrier;
        else
            b |= (tiledCache ? TiledCacheFlush : 0) | WaitForIdle | InvalidateTextureData;
    }
    if (c.scope & Control::Buffers)
        b |= FlushShaderData | InvalidateShaderData | InvalidateConstants;
    if (c.scope & Control::Textures)
        b |= FlushShaderData | InvalidateShaderData | InvalidateTextureData;
    return b;
}

std::vector<uint32_t> sampledSlots(const Recording &r, const Encoder &e)
{
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < e.ops.count; ++i) {
        const Op &op = r.ops[e.ops.first + i];
        if (op.kind != OpKind::Draw)
            continue;
        const DrawState &s = r.states[r.draws[op.payload].state];
        for (uint32_t k = 0; k < s.bindings.count; ++k) {
            const Binding &b = r.bindings[s.bindings.first + k];
            if (b.path == Path::Texture && b.resource != kNone &&
                std::find(out.begin(), out.end(), b.resource) == out.end())
                out.push_back(b.resource);
        }
    }
    return out;
}

// What a texture binding reads: an extended-range texture's stand-in, a shadowed texture's pool image, else the texture
// (raw::CommandBuffer::readTexture)
uint32_t readThrough(const Builder &b, const Binding &x)
{
    const ResourceFacts &f = b.facts(x.resource);
    if (x.path != Path::Texture)
        return x.resource;
    if (f.has(ResourceFacts::ExtendedRange) && f.standIn != kNone)
        return f.standIn;
    if (f.has(ResourceFacts::SampledShadow) && f.shadow != kNone)
        return f.shadow;
    return x.resource;
}

void stateUses(Builder &b, const Span &bindings)
{
    for (uint32_t k = 0; k < bindings.count; ++k) {
        const Binding &x = b.r.bindings[bindings.first + k];
        if (x.resource == kNone || b.facts(x.resource).kind == ResourceFacts::Sampler)
            continue;
        const uint32_t slot = readThrough(b, x);
        b.use(slot, x.path, false, stageOf(x.stage));
        if (x.write)
            b.use(slot, x.path, true, stageOf(x.stage));
    }
}

void startRun(Builder &b, const Draw &d, Barriers pending, uint32_t first)
{
    const DrawState &s = b.r.states[d.state];
    DrawRun &run = b.run(OwnState::None);
    run.state = d.state;
    run.program = s.program;
    run.draws = {first, 0};
    run.requested = pending;
    run.before = pending;
    bool storage = false, barrier = false;
    if (s.program != kNone) {
        const ProgramFacts &p = b.r.programs[s.program];
        for (uint32_t c = 0; c < 8; ++c) {
            if (!(p.fetchMask >> c & 1))
                continue;
            run.rung[c] = fetchRung(p.fetchRung[c], b.caps, run.rungRule[c]);
            storage |= run.rung[c] == Rung::Interlock;
            barrier |= run.rung[c] == Rung::Barrier;
        }
        run.variant = storage                                            ? Variant::Storage
                      : barrier && (p.forms & ProgramFacts::BarrierForm) ? Variant::Barrier
                                                                         : Variant::Whole;
    }
    stateUses(b, s.bindings);
}

void attachmentUses(Builder &b, const PassDesc &pass, uint8_t storageColours, uint8_t storageWrites)
{
    for (uint32_t j = 0; j < 8; ++j) {
        const Attachment &a = pass.color[j];
        if (a.texture == kNone)
            continue;
        const ResourceFacts &f = b.facts(a.texture);
        const uint32_t target = f.has(ResourceFacts::PlanarStandIn) ? f.standIn : a.texture;
        if (a.load == Load::Load)
            b.use(target, Path::Rop, false, Stage::Fragment, levelOf(a, pass.layers));
        b.use(target, Path::Rop, true, Stage::Fragment, levelOf(a, pass.layers));
        if (storageColours >> j & 1) {
            b.use(target, Path::Storage, false, Stage::Fragment, levelOf(a, pass.layers));
            if (storageWrites >> j & 1)
                b.use(target, Path::Storage, true, Stage::Fragment, levelOf(a, pass.layers));
        }
    }
    for (const Attachment *a : {&pass.depth, &pass.stencil}) {
        if (a->texture == kNone)
            continue;
        if (a->load == Load::Load)
            b.use(a->texture, Path::Zrop, false, Stage::Fragment, levelOf(*a, pass.layers));
        b.use(a->texture, Path::Zrop, true, Stage::Fragment, levelOf(*a, pass.layers));
    }
}

void runsOf(Builder &b, const Encoder &e, uint8_t &storageColours, uint8_t &storageWrites)
{
    Barriers pending = 0;
    DrawRun *open = nullptr;
    for (uint32_t i = 0; i < e.ops.count; ++i) {
        const Op &op = b.r.ops[e.ops.first + i];
        if (op.kind == OpKind::Control) {
            const Control &c = b.r.controls[op.payload];
            if (c.kind == Control::MemoryBarrier) {
                pending |= memoryBarrier(c, b.caps.tiledCache);
                open = nullptr;
            } else if (c.kind == Control::UseResource) {
                b.use(c.resource, b.facts(c.resource).kind == ResourceFacts::Texture ? Path::Texture : Path::Storage,
                      c.write);
            }
            continue;
        }
        if (op.kind != OpKind::Draw)
            continue;
        const Draw &d = b.r.draws[op.payload];
        if (!open || open->state != d.state) {
            startRun(b, d, pending, op.payload);
            pending = 0;
            open = &b.g.runs.back();
        }
        ++open->draws.count;
        b.use(d.indices, Path::Vertex, false, Stage::Vertex);
        b.use(d.arguments, Path::Indirect, false, Stage::Vertex);
        if (open->program != kNone) {
            const ProgramFacts &p = b.r.programs[open->program];
            for (uint32_t c = 0; c < 8; ++c)
                if (open->rung[c] == Rung::Interlock) {
                    storageColours |= uint8_t(1u << c);
                    storageWrites |= uint8_t(p.writeMask & (1u << c));
                }
        }
    }
}

bool clearOnly(const Recording &r, const Encoder &e)
{
    if (e.ops.count)
        return false;
    bool any = false;
    for (const Attachment &a : r.passes[e.pass].color) {
        if (a.texture == kNone)
            continue;
        if (a.load != Load::Clear || a.store != Store::Store)
            return false;
        any = true;
    }
    return any;
}

struct Walk {
    uint32_t lastPassNode = kNone, lastPassEncoder = kNone;
};

void render(Builder &b, uint32_t index, Walk &walk)
{
    const Encoder &e = b.r.encoders[index];
    const PassDesc &pass = b.r.passes[e.pass];
    const size_t before = b.g.nodes.size();
    beforePass(b, e, sampledSlots(b.r, e));
    const bool implicitBefore = b.g.nodes.size() != before;
    const bool clears = clearOnly(b.r, e);
    Choice clear;
    if (clears)
        for (const Attachment &a : pass.color)
            if (a.texture != kNone) {
                clear = clearEngine(b.facts(a.texture), b.caps);
                break;
            }
    const uint32_t n = b.open(clears ? NodeKind::Clear : NodeKind::RenderPass, clears ? clear.engine : Engine::ThreeD,
                              clears ? clear.rule : Rule::Recorded, index);
    b.g.nodes[n].pass = e.pass;
    uint8_t storageColours = 0, storageWrites = 0;
    runsOf(b, e, storageColours, storageWrites);
    attachmentUses(b, pass, storageColours, storageWrites);
    b.g.nodes[n].continues = !clears && !implicitBefore && walk.lastPassNode != kNone && walk.lastPassNode + 1 == n &&
                             walk.lastPassEncoder + 1 == index && fusible(b.r, b.r.encoders[index - 1], e);
    for (const Attachment &a : pass.color)
        if (a.texture != kNone)
            b.fresh[a.texture] = b.standInFresh[a.texture] = 0;
    afterPass(b, e);
    walk.lastPassNode = b.g.nodes.size() - 1 == n ? n : kNone;
    walk.lastPassEncoder = index;
}

void transfer(Builder &b, uint32_t at)
{
    const Transfer &t = b.r.transfers[at];
    if (t.kind == Transfer::Mipmaps) {
        mipChain(b, t, at);
        return;
    }
    const bool fill = t.kind == Transfer::FillBuffer;
    const Choice c = fill ? fillEngine(b.facts(t.destination), b.caps)
                          : copyEngine(b.facts(t.source), b.facts(t.destination), b.caps);
    const uint32_t n = b.open(fill ? NodeKind::Fill : NodeKind::Copy, c.valid() ? c.engine : Engine::Copy,
                              c.valid() ? c.rule : Rule::CopyEngine, at);
    Node &node = b.g.nodes[n];
    node.from = fill ? kNone : t.source;
    node.to = t.destination;
    node.fromRange = t.sourceRange;
    node.toRange = t.destinationRange;
    const bool twod = c.engine == Engine::TwoD;
    if (!fill)
        b.use(t.source, twod ? Path::Texture : Path::Copy, false, Stage::Transfer, t.sourceRange);
    b.use(t.destination, twod ? Path::TwoD : Path::Copy, true, Stage::Transfer, t.destinationRange);
    if (!c.valid())
        b.unplanned.push_back(n);
    b.fresh[t.destination] = b.standInFresh[t.destination] = 0;
}

void compute(Builder &b, uint32_t index)
{
    const Encoder &e = b.r.encoders[index];
    for (uint32_t i = 0; i < e.ops.count; ++i) {
        const Op &op = b.r.ops[e.ops.first + i];
        if (op.kind != OpKind::Dispatch)
            continue;
        const Dispatch &d = b.r.dispatches[op.payload];
        beforeDispatch(b, d);
        b.open(NodeKind::Dispatch, Engine::Compute, Rule::Recorded, op.payload);
        stateUses(b, d.bindings);
        afterDispatch(b, d);
    }
}

void boundary(Builder &b, uint32_t at)
{
    const Boundary &x = b.r.boundaries[at];
    switch (x.kind) {
    case Boundary::Wait:
        b.open(NodeKind::Wait, Engine::None, Rule::Recorded, at);
        break;
    case Boundary::Signal:
        b.open(NodeKind::Signal, Engine::None, Rule::Recorded, at);
        break;
    case Boundary::Present:
        b.open(NodeKind::Present, Engine::Display, Rule::Recorded, at);
        b.use(x.payload, Path::Display, false, Stage::Transfer);
        break;
    }
}

// The fences encoder `e` waits for (at its start) or updates (at its end): their controls' resources
std::vector<uint32_t> fencesOf(const Recording &r, const Encoder &e, Control::Kind kind)
{
    std::vector<uint32_t> out;
    for (uint32_t k = 0; k < e.ops.count; ++k)
        if (const Op &op = r.ops[e.ops.first + k]; op.kind == OpKind::Control && r.controls[op.payload].kind == kind)
            out.push_back(r.controls[op.payload].resource);
    return out;
}

}  // namespace

Built build(const Recording &r, const Caps &caps)
{
    Built out;
    Builder b(r, caps, out.graph);
    Walk walk;
    std::map<uint32_t, uint32_t> updated;  // fence: the last node of the encoder that last updated it
    const auto boundaries = [&](uint32_t after) {
        for (uint32_t i = 0; i < r.boundaries.size(); ++i)
            if (r.boundaries[i].afterEncoder == after)
                boundary(b, i);
    };
    boundaries(0);
    for (uint32_t i = 0; i < r.encoders.size(); ++i) {
        const Encoder &e = r.encoders[i];
        const uint32_t first = uint32_t(out.graph.nodes.size());
        std::vector<uint32_t> waited;
        for (uint32_t fence : fencesOf(r, e, Control::WaitFence))
            if (const auto it = updated.find(fence); it != updated.end())
                waited.push_back(it->second);
        if (!waited.empty())
            walk.lastPassNode = kNone;
        if (e.kind == EncoderKind::Render) {
            render(b, i, walk);
        } else {
            walk.lastPassNode = kNone;
            if (e.kind == EncoderKind::Blit) {
                for (uint32_t k = 0; k < e.ops.count; ++k)
                    if (r.ops[e.ops.first + k].kind == OpKind::Transfer)
                        transfer(b, r.ops[e.ops.first + k].payload);
            } else {
                compute(b, i);
            }
        }
        const size_t nodes = out.graph.nodes.size();
        if (nodes > first) {
            for (uint32_t updater : waited)
                out.graph.fences.push_back({first, updater});
            for (uint32_t fence : fencesOf(r, e, Control::UpdateFence))
                updated[fence] = uint32_t(nodes - 1);
        }
        boundaries(i + 1);
        if (out.graph.nodes.size() != nodes)
            walk.lastPassNode = kNone;
    }
    out.unplanned = std::move(b.unplanned);
    return out;
}

Built plan(const Recording &r, const Caps &caps, const JobContext &job)
{
    Built out = build(r, caps);
    placeBarriers(out.graph, r, caps, job);
    nodeKeys(out.graph, r);
    return out;
}

}  // namespace raw::plan
