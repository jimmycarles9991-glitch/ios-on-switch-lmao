// SPDX-License-Identifier: MIT
// The work encodeRawAttempt (RawMetalEncode.mm) invents while it emits, as named rules that make nodes before any word is
// written. Each rule cites the lambda it replaces.
#include "plan_builder.h"
#include <algorithm>

namespace raw::plan {

namespace {

bool sampledThroughShadow(const ResourceFacts &f)
{
    return f.has(ResourceFacts::SampledShadow) && f.shadow != kNone;
}

// encodeRawAttempt's planar block (YCbCr): plane shadows filled by the copy engine, then a pass of the pack's conversion state.
// No engine on the GPU channel converts 4:2:0 to RGB; VIC's colour conversion is a job on another engine
void ycbcr(Builder &b, uint32_t t)
{
    const ResourceFacts &f = b.facts(t);
    if (!f.has(ResourceFacts::Ycbcr) || !(f.has(ResourceFacts::ImageStale) || b.planesWritten[t]) || b.converted[t])
        return;
    for (uint32_t plane : {f.luma, f.chroma})
        if (plane != kNone && sampledThroughShadow(b.facts(plane)))
            b.copy(Rule::YcbcrPlaneFill, plane, b.facts(plane).shadow);
    const uint32_t luma = f.luma != kNone && sampledThroughShadow(b.facts(f.luma)) ? b.facts(f.luma).shadow : f.luma;
    b.ownPass(Rule::YcbcrImage, OwnState::Ycbcr, luma, t);
    if (f.chroma != kNone)
        b.use(sampledThroughShadow(b.facts(f.chroma)) ? b.facts(f.chroma).shadow : f.chroma, Path::Texture, false);
    b.converted[t] = 1;
}

// encodeRawAttempt's planar block and standInPass (convertPlanar): a 4:4:4 code texture's codes from its two planes, by copy-engine remaps
uint32_t planarCodes(Builder &b, uint32_t t)
{
    const ResourceFacts &f = b.facts(t);
    const uint32_t n = b.open(NodeKind::Convert, Engine::Copy, Rule::PlanarCodes);
    b.g.nodes[n].from = f.luma;
    b.g.nodes[n].to = t;
    b.use(f.luma, Path::Copy, false, Stage::Transfer);
    b.use(f.chroma, Path::Copy, false, Stage::Transfer);
    b.use(t, Path::Copy, true, Stage::Transfer);
    b.codes[t] = 1;
    return n;
}

// encodeRawAttempt's refresh: a sampled texture's pool image from its texels, once until work here writes it
// (a surface view at every pass)
void shadowRefresh(Builder &b, uint32_t t)
{
    const ResourceFacts &f = b.facts(t);
    if (!sampledThroughShadow(f) || (b.fresh[t] && !f.has(ResourceFacts::SurfaceView)))
        return;
    b.copy(Rule::ShadowRefresh, t, f.shadow);
    b.fresh[t] = 1;
}

// encodeRawAttempt's standIns: an extended-range texture's RGBA16F stand-in drawn from its codes. The texture header has no
// extended-range format; an UNORM view with the affine in the consumer's program (an L4 variant) would remove the pass
void extendedRange(Builder &b, uint32_t t)
{
    const ResourceFacts &f = b.facts(t);
    if (!f.has(ResourceFacts::ExtendedRange) || f.standIn == kNone || b.standInFresh[t])
        return;
    b.ownPass(Rule::ExtendedRangeStandIn, OwnState::ExtendedRange, t, f.standIn);
    b.standInFresh[t] = 1;
}

bool loadsStandIn(const Attachment &a)
{
    return a.load == Load::Load || (a.load == Load::DontCare && a.store != Store::DontCare);
}

}  // namespace

void beforePass(Builder &b, const Encoder &e, const std::vector<uint32_t> &sampled)
{
    for (uint32_t t : sampled) {
        const ResourceFacts &f = b.facts(t);
        ycbcr(b, t);
        if (f.has(ResourceFacts::PlanarCodes) && (f.has(ResourceFacts::ImageStale) || b.planesWritten[t]) &&
            !b.codes[t])
            planarCodes(b, t);
        shadowRefresh(b, t);
        extendedRange(b, t);
    }
    // encodeRawAttempt's standInPass(t, false): the planes' codes, then the stand-in drawn from them. The ROP writes no two
    // planes of different sizes and the copy engine's remap does not subsample, so the stand-in is drawn
    for (const Attachment &a : b.r.passes[e.pass].color) {
        if (a.texture == kNone || !b.facts(a.texture).has(ResourceFacts::PlanarStandIn) || !loadsStandIn(a))
            continue;
        planarCodes(b, a.texture);
        b.ownPass(Rule::PlanarStandInLoad, OwnState::StandInLoad, a.texture, b.facts(a.texture).standIn);
    }
}

void afterPass(Builder &b, const Encoder &e)
{
    const PassDesc &pass = b.r.passes[e.pass];
    // encodeRawAttempt's resolvePasses: each resolved attachment into its resolve texture
    for (const Attachment &a : pass.color) {
        if (a.texture == kNone || a.resolve == kNone ||
            (a.store != Store::Resolve && a.store != Store::StoreAndResolve))
            continue;
        const Choice c = resolveEngine(b.facts(a.texture), b.facts(a.resolve), b.caps);
        SubRange into;
        into.level = a.resolveLevel;
        into.levels = 1;
        if (c.engine == Engine::ThreeD) {
            const uint32_t n = b.ownPass(c.rule, OwnState::Resolve, a.texture, a.resolve, {}, into);
            b.g.nodes[n].kind = NodeKind::Resolve;
            continue;
        }
        const uint32_t n = b.open(NodeKind::Resolve, c.engine, c.valid() ? c.rule : Rule::ResolveDraw);
        b.g.nodes[n].from = a.texture;
        b.g.nodes[n].to = a.resolve;
        b.use(a.texture, Path::Texture, false);
        b.use(a.resolve, Path::TwoD, true, Stage::Transfer, into);
        if (!c.valid())
            b.unplanned.push_back(n);
    }
    // encodeRawAttempt's standInPass(t, true): the stand-in back into the planes
    for (const Attachment &a : pass.color) {
        if (a.texture == kNone || !b.facts(a.texture).has(ResourceFacts::PlanarStandIn) || a.store == Store::DontCare)
            continue;
        const ResourceFacts &f = b.facts(a.texture);
        b.ownPass(Rule::PlanarStandInStore, OwnState::StandInStore, f.standIn, f.luma);
        b.use(f.chroma, Path::Rop, true);
        b.codes[a.texture] = b.converted[a.texture] = 0;
        b.planesWritten[a.texture] = 1;
    }
}

std::vector<uint32_t> shadowedSlots(const Builder &b, const Dispatch &d)
{
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < d.bindings.count; ++i) {
        const Binding &x = b.r.bindings[d.bindings.first + i];
        if (x.path == Path::Texture && x.index < 32 && (d.imageShadowSlots >> x.index & 1) &&
            b.facts(x.resource).shadow != kNone)
            out.push_back(x.resource);
    }
    return out;
}

// encodeRawAttempt's copyShadows(false): the kernel's images from their textures before the dispatch
void beforeDispatch(Builder &b, const Dispatch &d)
{
    for (uint32_t t : shadowedSlots(b, d))
        b.copy(Rule::ComputeShadowLoad, t, b.facts(t).shadow);
}

// encodeRawAttempt's copyShadows(true): the images back into their textures, and every pool image stale again
void afterDispatch(Builder &b, const Dispatch &d)
{
    const std::vector<uint32_t> slots = shadowedSlots(b, d);
    for (uint32_t t : slots)
        b.copy(Rule::ComputeShadowStore, b.facts(t).shadow, t);
    if (!slots.empty())
        std::fill(b.fresh.begin(), b.fresh.end(), 0);
}

// encodeRawAttempt's OpMipmaps case and mipPasses (RawMetalEncodeBlit.mm): the 2D engine's chain, else a pass a level
void mipChain(Builder &b, const Transfer &t, uint32_t source)
{
    const uint32_t tex = t.destination != kNone ? t.destination : t.source;
    const ResourceFacts &f = b.facts(tex);
    const Choice c = mipEngine(f, b.caps);
    if (c.engine == Engine::ThreeD) {
        for (uint16_t l = 0; l + 1 < f.levels; ++l) {
            SubRange from, to;
            from.level = l;
            from.levels = 1;
            to.level = uint16_t(l + 1);
            to.levels = 1;
            const uint32_t n = b.ownPass(c.rule, OwnState::Mip, tex, tex, from, to);
            b.g.nodes[n].kind = NodeKind::MipChain;
            b.g.nodes[n].source = source;
        }
    } else {
        const uint32_t n =
            b.open(NodeKind::MipChain, c.valid() ? c.engine : Engine::TwoD, c.valid() ? c.rule : Rule::MipTwoD, source);
        SubRange from, to;
        from.levels = uint16_t(f.levels > 1 ? f.levels - 1 : 1);
        to.level = 1;
        to.levels = from.levels;
        b.g.nodes[n].from = b.g.nodes[n].to = tex;
        b.use(tex, Path::Texture, false, Stage::Transfer, from);
        b.use(tex, Path::TwoD, true, Stage::Transfer, to);
        if (!c.valid())
            b.unplanned.push_back(n);
    }
    b.fresh[tex] = 0;
    b.standInFresh[tex] = 0;
}

}  // namespace raw::plan
