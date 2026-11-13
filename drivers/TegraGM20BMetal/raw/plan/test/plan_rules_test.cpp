// SPDX-License-Identifier: MIT
// Every predicate predicates.tsv says a planner rule covers: its trigger, recorded, produces the rule's node or rung.
#include "../plan_lower.h"
#include "bazel/testing/source_text.h"
#include "fixtures.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <sstream>

namespace raw::plan::test {
namespace {

const std::set<std::string> kTested = {"tex.planar", "call.blitopt",  "blit.tex",    "blit.b2t", "blit.t2b",
                                       "blit.whole", "compute.image", "mipmaps.fmt", "event",    "enc.planarrt",
                                       "enc.pass",   "enc.geometry",  "enc.storage"};

bool has(const WorkGraph &g, Rule rule)
{
    return std::any_of(g.nodes.begin(), g.nodes.end(), [&](const Node &n) {
        return n.rule == rule;
    });
}

uint32_t index(const WorkGraph &g, Rule rule, uint32_t from = 0)
{
    for (uint32_t i = from; i < g.nodes.size(); ++i)
        if (g.nodes[i].rule == rule)
            return i;
    return kNone;
}

Transfer copy(Transfer::Kind kind, uint32_t from, uint32_t to, uint8_t aspect = 0)
{
    Transfer t;
    t.kind = kind;
    t.source = from;
    t.destination = to;
    t.aspect = aspect;
    return t;
}

Built blit(Rec &rec, const std::vector<Transfer> &transfers)
{
    rec.blit();
    for (const Transfer &t : transfers)
        rec.transfer(t);
    rec.end();
    return plan(rec.r, chipCaps(), JobContext{});
}

TEST(Rules, EveryRuleRowIsTested)
{
    std::istringstream in(switch_ios::test::SourceText("../predicates.tsv"));
    std::set<std::string> rows;
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        std::string tag, fires, fate;
        std::getline(fields, tag, '\t');
        std::getline(fields, fires, '\t');
        std::getline(fields, fate, '\t');
        if (fate == "rule")
            rows.insert(tag);
    }
    EXPECT_EQ(rows, kTested);
}

TEST(Rules, TexPlanar)
{
    Rec rec;
    const uint32_t luma = rec.texture(10, 64, 64), chroma = rec.texture(30, 64, 64), target = rec.texture(80, 64, 64);
    const uint32_t codes = rec.texture(70, 64, 64, ResourceFacts::PlanarCodes | ResourceFacts::ImageStale);
    rec.r.resources[codes].luma = luma;
    rec.r.resources[codes].chroma = chroma;
    rec.render(pass({colour(target, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(1, {80})), {texture(codes)}));
    rec.end();
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    ASSERT_EQ(b.graph.nodes[0].rule, Rule::PlanarCodes);
    EXPECT_EQ(b.graph.nodes[0].engine, Engine::Copy);
    EXPECT_EQ(b.graph.nodes[1].before, EngineSwitch | WaitForIdle | InvalidateTextureData) << "the codes, then sampled";
}

TEST(Rules, CallBlitOptions)
{
    Rec rec;
    const uint32_t staging = rec.buffer(1 << 16), zeta = rec.texture(260, 64, 64);
    const Built b = blit(rec, {copy(Transfer::CopyBufferToTexture, staging, zeta, 2)});
    EXPECT_EQ(b.graph.nodes[0].rule, Rule::CopyEngine);
}

TEST(Rules, BlitTextureCopiesOfLevelsAndSlices)
{
    Rec rec;
    const uint32_t a = rec.texture(80, 256, 256, 0, 4), b = rec.texture(80, 256, 256, 0, 4);
    Transfer t = copy(Transfer::CopyTexture, a, b);
    t.sourceRange = {2, 1, 1, 1};
    t.destinationRange = {1, 1, 0, 1};
    const Built built = blit(rec, {t});
    EXPECT_EQ(built.graph.nodes[0].rule, Rule::CopyEngine);
    EXPECT_EQ(built.graph.nodes[0].toRange.level, 1u);
}

TEST(Rules, BlitBufferToTextureAndBack)
{
    Rec rec;
    const uint32_t buffer = rec.buffer(1 << 20), image = rec.texture(115, 128, 128);
    const Built b = blit(
        rec, {copy(Transfer::CopyBufferToTexture, buffer, image), copy(Transfer::CopyTextureToBuffer, image, buffer)});
    EXPECT_EQ(b.graph.nodes[0].rule, Rule::CopyEngine);
    EXPECT_EQ(b.graph.nodes[1].rule, Rule::CopyEngine);
    EXPECT_EQ(b.graph.nodes[1].before, CopyNonPipelined) << "the copy engine orders its own copies";
}

TEST(Rules, BlitWholeTextureCopyIsACopyALevel)
{
    Rec rec;
    const uint32_t a = rec.texture(80, 256, 256, 0, 4), b = rec.texture(80, 256, 256, 0, 4);
    const Built built = blit(rec, {copy(Transfer::CopyTexture, a, b)});
    const std::vector<EmitOp> ops = lower(built.graph, rec.r, JobContext{});
    std::vector<uint16_t> levels;
    for (const EmitOp &op : ops)
        if (op.emitter == Emitter::CopyRect)
            levels.push_back(op.level);
    EXPECT_EQ(levels, (std::vector<uint16_t>{0, 1, 2, 3}));
}

TEST(Rules, ComputeImage)
{
    Rec rec;
    const uint32_t pool = rec.texture(70, 64, 64), image = rec.texture(71, 64, 64, ResourceFacts::SampledShadow);
    rec.r.resources[image].shadow = pool;
    rec.compute();
    rec.dispatch(rec.program(ProgramFacts{}), {texture(image, 2, 0)}, 1);
    rec.end();
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    EXPECT_TRUE(has(b.graph, Rule::ComputeShadowLoad));
    EXPECT_TRUE(has(b.graph, Rule::ComputeShadowStore));
}

TEST(Rules, MipmapsOfEveryFormatWithARung)
{
    Rec rec;
    const uint32_t twod = rec.texture(80, 64, 64, ResourceFacts::MipChain2D, 7), srgb = rec.texture(81, 64, 64, 0, 7);
    Transfer m1, m2;
    m1.kind = m2.kind = Transfer::Mipmaps;
    m1.destination = twod;
    m2.destination = srgb;
    const Built b = blit(rec, {m1, m2});
    EXPECT_TRUE(has(b.graph, Rule::MipTwoD));
    EXPECT_TRUE(has(b.graph, Rule::MipDraw));
    EXPECT_TRUE(has(build(rec.r, chipCaps(1u << uint32_t(Probe::MipFilter))).graph, Rule::MipTwoDProbed));
}

TEST(Rules, Events)
{
    Rec rec;
    const uint32_t event = rec.sampler(), target = rec.texture(80, 64, 64);
    rec.wait(event, 1);
    rec.render(pass({colour(target, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(1, {80})), {}));
    rec.end();
    rec.signal(event, 2);
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    EXPECT_EQ(b.graph.nodes.front().kind, NodeKind::Wait);
    EXPECT_EQ(b.graph.nodes.back().kind, NodeKind::Signal);
}

// A planar target rendered through its stand-in, then sampled in the same command buffer: its codes are made again from
// the planes the store wrote, after that store
TEST(Rules, EncPlanarRenderedThenSampled)
{
    Rec rec;
    const uint32_t luma = rec.texture(10, 64, 64), chroma = rec.texture(30, 32, 32), standIn = rec.texture(70, 64, 64);
    const uint32_t planar = rec.texture(540, 64, 64, ResourceFacts::PlanarStandIn | ResourceFacts::PlanarCodes);
    const uint32_t target = rec.texture(80, 64, 64);
    rec.r.resources[planar].luma = luma;
    rec.r.resources[planar].chroma = chroma;
    rec.r.resources[planar].standIn = standIn;
    rec.render(pass({colour(planar, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(1, {540})), {}));
    rec.end();
    rec.render(pass({colour(target, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(2, {80})), {texture(planar)}));
    rec.end();
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    const uint32_t store = index(b.graph, Rule::PlanarStandInStore);
    const uint32_t codes = index(b.graph, Rule::PlanarCodes);
    ASSERT_NE(store, kNone);
    ASSERT_NE(codes, kNone);
    EXPECT_LT(store, codes);
    const Node &n = b.graph.nodes[codes];
    EXPECT_TRUE(std::find(b.graph.edges.begin() + n.after.first, b.graph.edges.begin() + n.after.first + n.after.count,
                          store) != b.graph.edges.begin() + n.after.first + n.after.count);
}

TEST(Rules, EncPassImplicitTargets)
{
    Rec rec;
    const uint32_t standIn = rec.texture(115, 64, 64), xr = rec.texture(552, 64, 64, ResourceFacts::ExtendedRange);
    const uint32_t ms = rec.texture(80, 64, 64, ResourceFacts::Memoryless, 1, 4), into = rec.texture(80, 64, 64);
    rec.r.resources[xr].standIn = standIn;
    Attachment a = colour(ms, Load::Clear, Store::Resolve);
    a.resolve = into;
    rec.render(pass({a}, 64, 64));
    ProgramFacts p = caState(1, {80});
    p.samples = 4;
    rec.draw(rec.state(rec.program(p), {texture(xr)}));
    rec.end();
    const Built b = build(rec.r, chipCaps());
    EXPECT_TRUE(has(b.graph, Rule::ExtendedRangeStandIn));
    EXPECT_TRUE(has(b.graph, Rule::ResolveDraw));
}

// A draw that fetches the colour it blends, in the storage form, whatever its geometry: no vertex is read
TEST(Rules, EncGeometryAnyGeometry)
{
    Rec rec;
    const uint32_t target = rec.texture(80, 64, 64), args = rec.buffer(64), indices = rec.buffer(4096);
    const uint32_t s = rec.state(rec.program(caState(1, {80}, 1, Rung::Interlock)), {});
    rec.render(pass({colour(target, Load::Load, Store::Store)}, 64, 64));
    rec.draw(s, 900, 2, indices);
    rec.r.draws.back().primitive = 4;
    rec.r.draws.back().instances = 7;
    rec.drawIndirect(s, args);
    rec.end();
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    const DrawRun &run = b.graph.runs[b.graph.nodes[0].runs.first];
    EXPECT_EQ(run.rung[0], Rung::Interlock);
    EXPECT_EQ(run.rungRule[0], Rule::FetchInterlock);
    EXPECT_EQ(run.variant, Variant::Storage);
    EXPECT_EQ(run.draws.count, 2u);
}

// A draw reading a colour an earlier draw of the pass wrote, which it does not write: the barrier form behind a pixel
// barrier
TEST(Rules, EncStorageBarrierForm)
{
    Rec rec;
    const uint32_t c0 = rec.texture(80, 64, 64), c1 = rec.texture(115, 64, 64);
    rec.render(pass({colour(c0, Load::Load, Store::Store), colour(c1, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(1, {80, 115})), {}));
    ProgramFacts reader = caState(2, {80, 115}, 2, Rung::Barrier);
    reader.writeMask = 1;
    rec.draw(rec.state(rec.program(reader), {}));
    rec.end();
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    const DrawRun &run = b.graph.runs[b.graph.nodes[0].runs.first + 1];
    EXPECT_EQ(run.variant, Variant::Barrier);
    EXPECT_EQ(run.rungRule[1], Rule::FetchBarrier);
    EXPECT_EQ(run.before, PixelShaderBarrier);
}

}  // namespace
}  // namespace raw::plan::test
