// SPDX-License-Identifier: MIT
// The hazard table, row by row, and its placement: a barrier goes where an edge needs it and a barrier already placed since
// the earlier access satisfies it.
#include "../plan_hazard.h"
#include "fixtures.h"
#include <gtest/gtest.h>

namespace raw::plan::test {
namespace {

Access colourAt(bool write = true)
{
    return {Path::Rop, write, Stage::Fragment, Engine::ThreeD};
}

Access sampled(Stage stage = Stage::Fragment)
{
    return {Path::Texture, false, stage, Engine::ThreeD};
}

Access stored(bool write, Engine e = Engine::ThreeD)
{
    return {Path::Storage, write, Stage::Fragment, e};
}

Access copied(bool write)
{
    return {Path::Copy, write, Stage::Transfer, Engine::Copy};
}

Barriers across(const Access &a, const Access &b, bool tiled = true)
{
    return hazard(a, b, Scope::AcrossNodes, tiled).barriers;
}

TEST(Hazard, ColourWriteThenSample)
{
    const Requirement r = hazard(colourAt(), sampled(), Scope::AcrossNodes, true);
    EXPECT_EQ(r.barriers, TiledCacheFlush | WaitForIdle | InvalidateTextureData);
    EXPECT_EQ(r.relaxed, TiledCacheFlush | PixelShaderBarrier) << "probe-gated: pixel-ordered across passes";
    EXPECT_EQ(across(colourAt(), sampled(), false), WaitForIdle | InvalidateTextureData);
    EXPECT_EQ(hazard(colourAt(), sampled(Stage::Vertex), Scope::AcrossNodes, true).relaxed, 0) << "a vertex-stage read";
}

TEST(Hazard, StorageWritesFlush)
{
    EXPECT_EQ(across(stored(true), sampled()), FlushShaderData | InvalidateTextureData);
    EXPECT_EQ(across(stored(true), stored(false)), FlushShaderData | InvalidateShaderData);
    EXPECT_EQ(across(stored(true), {Path::Constant, false, Stage::Vertex, Engine::ThreeD}),
              FlushShaderData | InvalidateConstants | InvalidateShaderData);
    EXPECT_EQ(across(stored(true), {Path::Indirect, false, Stage::Vertex, Engine::ThreeD}),
              FlushShaderData | SetReference | InvalidateConstants);
    EXPECT_EQ(across(stored(true), colourAt(false)), FlushShaderData) << "a load of what the interlock stored";
}

TEST(Hazard, CopyEngine)
{
    EXPECT_EQ(across(copied(true), copied(false)), CopyNonPipelined);
    EXPECT_EQ(across(copied(true), sampled()), WaitForIdle | InvalidateTextureData);
    EXPECT_EQ(across(colourAt(), copied(false)), TiledCacheFlush | WaitForIdle) << "the copy engine reads L2";
    EXPECT_EQ(across(copied(true), {Path::Vertex, false, Stage::Vertex, Engine::ThreeD}), WaitForIdle);
}

TEST(Hazard, WriteAfterReadIsOrderAlone)
{
    EXPECT_EQ(across(sampled(), colourAt()), WaitForIdle);
    EXPECT_EQ(hazard(sampled(), colourAt(), Scope::AcrossNodes, true).relaxed, PixelShaderBarrier);
    EXPECT_TRUE(hazard(sampled(), sampled(), Scope::AcrossNodes, true).none());
}

TEST(Hazard, InsideAPass)
{
    EXPECT_EQ(hazard(colourAt(), sampled(), Scope::InPass, true).barriers, PixelShaderBarrier);
    EXPECT_EQ(hazard(colourAt(), stored(false), Scope::InPass, true).barriers,
              TiledCacheFlush | FlushShaderData | InvalidateShaderData | InvalidateTextureData);
    const Requirement ticket = hazard(stored(true), stored(true), Scope::InPass, true);
    EXPECT_TRUE(ticket.none());
    EXPECT_TRUE(ticket.clauses & (1u << uint32_t(Clause::InterlockOrder)));
    EXPECT_TRUE(hazard(colourAt(), colourAt(), Scope::InPass, true).none()) << "the ROP keeps API order";
}

TEST(Hazard, PresentNeedsWritesOutOfCaches)
{
    const Access display{Path::Display, false, Stage::Transfer, Engine::Display};
    EXPECT_EQ(across(colourAt(), display), TiledCacheFlush);
    EXPECT_EQ(across(stored(true), display), FlushShaderData);
}

TEST(Hazard, EveryClauseCitesNvkAndRaw)
{
    for (size_t i = 0; i < size_t(Clause::Count); ++i) {
        const ClauseInfo &c = clauseInfo(Clause(i));
        EXPECT_EQ(size_t(c.clause), i);
        EXPECT_GT(std::string(c.nvk).size(), 8u) << c.name;
        EXPECT_GT(std::string(c.raw).size(), 3u) << c.name;
    }
}

TEST(Hazard, EngineSwitchDrains)
{
    EXPECT_TRUE(switchDrains(Engine::ThreeD, Engine::Copy));
    EXPECT_TRUE(switchDrains(Engine::Copy, Engine::Compute));
    EXPECT_FALSE(switchDrains(Engine::ThreeD, Engine::ThreeD));
    EXPECT_FALSE(switchDrains(Engine::ThreeD, Engine::Display));
}

// A copy into a texture, then a pass sampling it: the WFI stays until the probe shows the subchannel change to the 3D
// engine drains the copy engine; then the switch is the drain and only the texture lines are dropped
TEST(Placement, CopyThenSampleTakesTheSwitchAsItsDrainOnceProbed)
{
    Rec rec;
    const uint32_t staging = rec.buffer(1 << 16), image = rec.texture(80, 64, 64), target = rec.texture(80, 64, 64);
    rec.blit();
    Transfer t;
    t.kind = Transfer::CopyBufferToTexture;
    t.source = staging;
    t.destination = image;
    rec.transfer(t);
    rec.end();
    rec.render(pass({colour(target, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(1, {80})), {texture(image)}));
    rec.end();
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    ASSERT_EQ(b.graph.nodes.size(), 2u);
    EXPECT_EQ(b.graph.nodes[1].before, EngineSwitch | WaitForIdle | InvalidateTextureData);
    const Built probed = plan(rec.r, chipCaps(1u << uint32_t(Probe::SwitchDrain)), JobContext{});
    EXPECT_EQ(probed.graph.nodes[1].before, EngineSwitch | InvalidateTextureData);
}

// Two passes sampling what one pass drew: the second needs nothing the first's barrier did not give
TEST(Placement, ABarrierSatisfiesLaterReaders)
{
    Rec rec;
    const uint32_t a = rec.texture(80, 64, 64), b = rec.texture(80, 64, 64), c = rec.texture(80, 64, 64);
    const uint32_t p = rec.program(caState(1, {80}));
    rec.render(pass({colour(a, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(p, {}));
    rec.end();
    for (uint32_t t : {b, c}) {
        rec.render(pass({colour(t, Load::Clear, Store::Store)}, 64, 64));
        rec.draw(rec.state(p, {texture(a)}));
        rec.end();
    }
    const Built built = plan(rec.r, chipCaps(), JobContext{});
    EXPECT_EQ(built.graph.nodes[1].before, TiledCacheFlush | WaitForIdle | InvalidateTextureData);
    EXPECT_EQ(built.graph.nodes[2].before, 0);
}

TEST(Placement, JobEntryDrainsUntilProbed)
{
    const Recording r = glassFrame1();
    EXPECT_TRUE(plan(r, chipCaps(), JobContext{}).graph.entry & FlushShaderData);
    const Barriers probed = plan(r, chipCaps(1u << uint32_t(Probe::IdleJobBoundary)), JobContext{}).graph.entry;
    EXPECT_FALSE(probed & (FlushShaderData | WaitForIdle));
    EXPECT_TRUE(probed & InvalidateTextureData);
}

TEST(Placement, InstructionCachesOnlyWhenTheHeapGrew)
{
    const Recording r = glassFrame1();
    EXPECT_FALSE(plan(r, chipCaps(), JobContext{}).graph.entry & InvalidateInstructions);
    EXPECT_TRUE(plan(r, chipCaps(), JobContext{true}).graph.entry & InvalidateInstructions);
}

TEST(Placement, GpuWrittenConstants)
{
    Rec rec;
    const uint32_t uniforms = rec.buffer(256), target = rec.texture(80, 64, 64);
    rec.compute();
    rec.dispatch(rec.program(ProgramFacts{}), {storage(uniforms, true)});
    rec.end();
    rec.render(pass({colour(target, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(1, {80})), {constants(uniforms)}));
    rec.end();
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    EXPECT_EQ(b.graph.nodes[1].before, FlushShaderData | InvalidateConstants | InvalidateShaderData | EngineSwitch);
}

}  // namespace
}  // namespace raw::plan::test
