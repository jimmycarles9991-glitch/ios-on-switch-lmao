// SPDX-License-Identifier: MIT
// The builder: each implicit pass encodeRawAttempt makes today is a node a named rule makes, at the place the encode loop
// makes it; draw runs carry the fetch rung and variant; fused passes are marked.
#include "fixtures.h"
#include <gtest/gtest.h>
#include <algorithm>

namespace raw::plan::test {
namespace {

std::vector<Rule> rules(const WorkGraph &g)
{
    std::vector<Rule> out;
    for (const Node &n : g.nodes)
        out.push_back(n.rule);
    return out;
}

struct OnePass {
    Rec rec;
    uint32_t target, program;

    OnePass()
    {
        target = rec.texture(80, 64, 64);
        program = rec.program(caState(1, {80}));
    }

    void samplingPass(uint32_t sampled, Load load = Load::Clear, Store store = Store::Store)
    {
        rec.render(pass({colour(target, load, store)}, 64, 64));
        rec.draw(rec.state(program, {texture(sampled)}));
        rec.end();
    }
};

TEST(Build, ExtendedRangeStandInOnceUntilWritten)
{
    OnePass f;
    const uint32_t standIn = f.rec.texture(115, 64, 64);
    const uint32_t xr = f.rec.texture(550, 64, 64, ResourceFacts::ExtendedRange);
    f.rec.r.resources[xr].standIn = standIn;
    f.samplingPass(xr);
    f.samplingPass(xr);
    f.rec.render(pass({colour(xr, Load::Load, Store::Store)}, 64, 64));
    f.rec.draw(f.rec.state(f.program, {}));
    f.rec.end();
    f.samplingPass(xr);
    const Built b = build(f.rec.r, chipCaps());
    EXPECT_EQ(rules(b.graph), (std::vector<Rule>{Rule::ExtendedRangeStandIn, Rule::Recorded, Rule::Recorded,
                                                 Rule::Recorded, Rule::ExtendedRangeStandIn, Rule::Recorded}));
    EXPECT_EQ(b.graph.nodes[0].engine, Engine::ThreeD);
    EXPECT_EQ(b.graph.runs[b.graph.nodes[0].runs.first].own, OwnState::ExtendedRange);
}

TEST(Build, SampledShadowRefreshedOnceASurfaceViewEachPass)
{
    OnePass f;
    const uint32_t pool = f.rec.texture(10, 360, 240);
    const uint32_t thumb = f.rec.texture(10, 360, 240, ResourceFacts::SampledShadow);
    const uint32_t view = f.rec.texture(10, 360, 240, ResourceFacts::SampledShadow | ResourceFacts::SurfaceView);
    f.rec.r.resources[thumb].shadow = pool;
    f.rec.r.resources[view].shadow = pool;
    f.samplingPass(thumb);
    f.samplingPass(thumb);
    f.samplingPass(view);
    f.samplingPass(view);
    const Built b = build(f.rec.r, chipCaps());
    EXPECT_EQ(rules(b.graph),
              (std::vector<Rule>{Rule::ShadowRefresh, Rule::Recorded, Rule::Recorded, Rule::ShadowRefresh,
                                 Rule::Recorded, Rule::ShadowRefresh, Rule::Recorded}));
    EXPECT_EQ(b.graph.nodes[0].engine, Engine::Copy);
}

TEST(Build, YcbcrImageWhenStale)
{
    OnePass f;
    const uint32_t lumaPool = f.rec.texture(10, 360, 240);
    const uint32_t luma = f.rec.texture(10, 360, 240, ResourceFacts::SampledShadow | ResourceFacts::Surface);
    const uint32_t chroma = f.rec.texture(30, 180, 120, ResourceFacts::Surface);
    const uint32_t image = f.rec.texture(500, 360, 240, ResourceFacts::Ycbcr | ResourceFacts::ImageStale);
    f.rec.r.resources[luma].shadow = lumaPool;
    f.rec.r.resources[image].luma = luma;
    f.rec.r.resources[image].chroma = chroma;
    f.samplingPass(image);
    f.samplingPass(image);
    const Built b = build(f.rec.r, chipCaps());
    EXPECT_EQ(rules(b.graph),
              (std::vector<Rule>{Rule::YcbcrPlaneFill, Rule::YcbcrImage, Rule::Recorded, Rule::Recorded}));
    f.rec.r.resources[image].flags &= ~uint32_t(ResourceFacts::ImageStale);
    EXPECT_EQ(rules(build(f.rec.r, chipCaps()).graph), (std::vector<Rule>{Rule::Recorded, Rule::Recorded}));
}

TEST(Build, PlanarStandInLoadsAndStores)
{
    Rec rec;
    const uint32_t luma = rec.texture(10, 64, 64), chroma = rec.texture(30, 32, 32), standIn = rec.texture(70, 64, 64);
    const uint32_t planar = rec.texture(540, 64, 64, ResourceFacts::PlanarStandIn | ResourceFacts::PlanarCodes);
    rec.r.resources[planar].luma = luma;
    rec.r.resources[planar].chroma = chroma;
    rec.r.resources[planar].standIn = standIn;
    rec.render(pass({colour(planar, Load::Load, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(1, {540})), {}));
    rec.end();
    const Built b = build(rec.r, chipCaps());
    EXPECT_EQ(rules(b.graph), (std::vector<Rule>{Rule::PlanarCodes, Rule::PlanarStandInLoad, Rule::Recorded,
                                                 Rule::PlanarStandInStore}));
    EXPECT_EQ(b.graph.nodes[0].engine, Engine::Copy);
}

TEST(Build, ResolveAfterThePass)
{
    Rec rec;
    const uint32_t ms = rec.texture(80, 64, 64, ResourceFacts::Memoryless, 1, 4), into = rec.texture(80, 64, 64);
    Attachment a = colour(ms, Load::Clear, Store::Resolve);
    a.resolve = into;
    rec.render(pass({a}, 64, 64));
    ProgramFacts p = caState(1, {80});
    p.samples = 4;
    rec.draw(rec.state(rec.program(p), {}));
    rec.end();
    Built b = build(rec.r, chipCaps());
    ASSERT_EQ(b.graph.nodes.size(), 2u);
    EXPECT_EQ(b.graph.nodes[1].kind, NodeKind::Resolve);
    EXPECT_EQ(b.graph.nodes[1].rule, Rule::ResolveDraw);
    b = build(rec.r, chipCaps(1u << uint32_t(Probe::TwodResolve)));
    EXPECT_EQ(b.graph.nodes[1].rule, Rule::ResolveTwoD);
    EXPECT_EQ(b.graph.nodes[1].engine, Engine::TwoD);
}

TEST(Build, MipmapsAChainOrAPassALevel)
{
    Rec rec;
    const uint32_t twod = rec.texture(80, 256, 256, ResourceFacts::MipChain2D, 9);
    const uint32_t srgb = rec.texture(81, 256, 256, 0, 9);
    rec.blit();
    for (uint32_t t : {twod, srgb}) {
        Transfer m;
        m.kind = Transfer::Mipmaps;
        m.destination = t;
        rec.transfer(m);
    }
    rec.end();
    const Built b = build(rec.r, chipCaps());
    ASSERT_EQ(b.graph.nodes.size(), 9u);
    EXPECT_EQ(b.graph.nodes[0].rule, Rule::MipTwoD);
    for (uint32_t i = 1; i < 9; ++i) {
        EXPECT_EQ(b.graph.nodes[i].rule, Rule::MipDraw);
        EXPECT_EQ(b.graph.nodes[i].fromRange.level + 1, b.graph.nodes[i].toRange.level);
    }
}

TEST(Build, ComputeShadowsAroundTheDispatch)
{
    Rec rec;
    const uint32_t pool = rec.texture(70, 64, 64), image = rec.texture(71, 64, 64, ResourceFacts::SampledShadow);
    rec.r.resources[image].shadow = pool;
    rec.compute();
    rec.dispatch(rec.program(ProgramFacts{}), {texture(image, 2, 1)}, 1u << 1);
    rec.end();
    const Built b = build(rec.r, chipCaps());
    EXPECT_EQ(rules(b.graph), (std::vector<Rule>{Rule::ComputeShadowLoad, Rule::Recorded, Rule::ComputeShadowStore}));
}

TEST(Build, RunsCarryRungAndVariant)
{
    const Built b = build(glassFrame1(), chipCaps());
    const Node &pass = b.graph.nodes[0];
    ASSERT_EQ(pass.runs.count, 3u);
    const DrawRun &merge = b.graph.runs[pass.runs.first + 2];
    EXPECT_EQ(merge.variant, Variant::Storage);
    EXPECT_EQ(merge.rung[0], Rung::Interlock);
    EXPECT_EQ(merge.rungRule[0], Rule::FetchInterlock);
    EXPECT_EQ(merge.draws.count, 2u);
    EXPECT_EQ(b.graph.runs[pass.runs.first].draws.count, 21u);
}

TEST(Build, MemoryBarrierSplitsARun)
{
    OnePass f;
    const uint32_t s = f.rec.state(f.program, {});
    f.rec.render(pass({colour(f.target, Load::Clear, Store::Store)}, 64, 64));
    f.rec.draw(s);
    Control c;
    c.scope = Control::RenderTargets;
    c.after = c.before = 2;
    f.rec.control(c);
    f.rec.draw(s);
    f.rec.end();
    const Built b = build(f.rec.r, chipCaps());
    ASSERT_EQ(b.graph.nodes[0].runs.count, 2u);
    EXPECT_EQ(b.graph.runs[1].requested, PixelShaderBarrier);
}

TEST(Build, FusesAPassThatContinuesTheOneBefore)
{
    OnePass f;
    const uint32_t s = f.rec.state(f.program, {});
    f.rec.render(pass({colour(f.target, Load::Clear, Store::Store)}, 64, 64));
    f.rec.draw(s);
    f.rec.end();
    f.rec.render(pass({colour(f.target, Load::Load, Store::Store)}, 64, 64));
    f.rec.draw(s);
    f.rec.end();
    f.samplingPass(f.target, Load::Load);
    const Built b = plan(f.rec.r, chipCaps(), JobContext{});
    EXPECT_TRUE(b.graph.nodes[1].continues);
    EXPECT_EQ(b.graph.nodes[1].before, 0);
    EXPECT_FALSE(b.graph.nodes[2].continues) << "it samples its own target";
}

Control fence(Control::Kind kind, uint32_t id)
{
    Control c;
    c.kind = kind;
    c.resource = id;
    return c;
}

TEST(Build, AFenceWaitDrainsBehindItsUpdate)
{
    OnePass f;
    const uint32_t written = f.rec.buffer(256);
    f.rec.blit();
    Transfer t;
    t.kind = Transfer::FillBuffer;
    t.destination = written;
    t.bytes = 256;
    f.rec.transfer(t);
    f.rec.control(fence(Control::UpdateFence, 7));
    f.rec.end();
    f.rec.render(pass({colour(f.target, Load::Clear, Store::Store)}, 64, 64));
    f.rec.control(fence(Control::WaitFence, 7));
    f.rec.draw(f.rec.state(f.program, {}));
    f.rec.end();
    const Built b = plan(f.rec.r, chipCaps(), JobContext{});
    ASSERT_EQ(b.graph.fences.size(), 1u);
    const Node &waiter = b.graph.nodes[b.graph.fences[0].waiter];
    EXPECT_EQ(waiter.kind, NodeKind::RenderPass);
    EXPECT_EQ(waiter.before & kFenceWait, kFenceWait);
    const uint32_t *edges = b.graph.edges.data() + waiter.after.first;
    EXPECT_NE(std::find(edges, edges + waiter.after.count, b.graph.fences[0].updater), edges + waiter.after.count);
}

TEST(Build, AFenceNothingUpdatedOrdersNothing)
{
    OnePass f;
    f.rec.render(pass({colour(f.target, Load::Clear, Store::Store)}, 64, 64));
    f.rec.control(fence(Control::WaitFence, 9));
    f.rec.draw(f.rec.state(f.program, {}));
    f.rec.end();
    const Built b = plan(f.rec.r, chipCaps(), JobContext{});
    EXPECT_TRUE(b.graph.fences.empty());
}

TEST(Build, APassThatWaitsForAFenceIsNeverFused)
{
    OnePass f;
    const uint32_t s = f.rec.state(f.program, {});
    f.rec.render(pass({colour(f.target, Load::Clear, Store::Store)}, 64, 64));
    f.rec.draw(s);
    f.rec.control(fence(Control::UpdateFence, 3));
    f.rec.end();
    f.rec.render(pass({colour(f.target, Load::Load, Store::Store)}, 64, 64));
    f.rec.control(fence(Control::WaitFence, 3));
    f.rec.draw(s);
    f.rec.end();
    const Built b = plan(f.rec.r, chipCaps(), JobContext{});
    ASSERT_EQ(b.graph.nodes.size(), 2u);
    EXPECT_FALSE(b.graph.nodes[1].continues);
    EXPECT_EQ(b.graph.nodes[1].before & kFenceWait, kFenceWait);
}

TEST(Build, AClearOnlyPassIsAClearNode)
{
    Rec rec;
    const uint32_t pitch = rec.texture(80, 64, 64, ResourceFacts::Drawable);
    rec.render(pass({colour(pitch, Load::Clear, Store::Store)}, 64, 64));
    rec.end();
    Built b = build(rec.r, chipCaps());
    ASSERT_EQ(b.graph.nodes.size(), 1u);
    EXPECT_EQ(b.graph.nodes[0].kind, NodeKind::Clear);
    EXPECT_EQ(b.graph.nodes[0].rule, Rule::ClearThreeD);
    b = build(rec.r, chipCaps(1u << uint32_t(Probe::TwodFill)));
    EXPECT_EQ(b.graph.nodes[0].rule, Rule::ClearTwoD);
    EXPECT_EQ(b.graph.nodes[0].engine, Engine::TwoD);
}

}  // namespace
}  // namespace raw::plan::test
