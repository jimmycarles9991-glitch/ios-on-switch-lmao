// SPDX-License-Identifier: MIT
// Lowering as data: barriers become the kit's sequences in its order, one op per barrier the counts name; nodes become
// their emitters; the ops outside the kit are the wiring agent's list of what raw keeps emitting for now.
#include "../plan_lower.h"
#include "fixtures.h"
#include <gtest/gtest.h>
#include <set>

namespace raw::plan::test {
namespace {

uint32_t count(const std::vector<EmitOp> &ops, Emitter e)
{
    uint32_t n = 0;
    for (const EmitOp &op : ops)
        n += op.emitter == e;
    return n;
}

TEST(Lower, BarriersInKitOrder)
{
    std::vector<EmitOp> ops;
    lowerBarriers(ops, TiledCacheFlush | FlushShaderData | InvalidateShaderData | InvalidateTextureData, 0,
                  Engine::ThreeD);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0].emitter, Emitter::TiledCacheFlush);
    EXPECT_EQ(ops[1].emitter, Emitter::InvalidateShaderCaches);
    EXPECT_EQ(ops[1].arg, uint32_t(FlushShaderData | InvalidateShaderData));
    EXPECT_EQ(ops[2].emitter, Emitter::InvalidateTextureData);
    ops.clear();
    lowerBarriers(ops, WaitForIdle | InvalidateTextureData, 0, Engine::ThreeD);
    EXPECT_EQ(ops[0].emitter, Emitter::WaitForIdle);
    ops.clear();
    lowerBarriers(ops, EngineSwitch | InvalidateTextureData, 0, Engine::ThreeD);
    ASSERT_EQ(ops.size(), 1u) << "the switch is not emitted";
}

TEST(Lower, OpsMatchTheCounts)
{
    for (const Recording &r : {glassFrame0(), glassFrame1(), scrollFrame()}) {
        const Built b = plan(r, chipCaps(), JobContext{});
        const std::vector<EmitOp> ops = lower(b.graph, r, JobContext{});
        const Counts c = countGraph(b.graph);
        EXPECT_EQ(count(ops, Emitter::WaitForIdle), c.wfi);
        EXPECT_EQ(count(ops, Emitter::InvalidateShaderCaches), c.shaderInval);
        EXPECT_EQ(count(ops, Emitter::InvalidateShaderCachesNoWfi), c.shaderInvalNoWfi);
        EXPECT_EQ(count(ops, Emitter::TiledCacheFlush), c.tiledFlushes);
        EXPECT_EQ(count(ops, Emitter::PixelShaderBarrier), c.psBarriers);
        EXPECT_EQ(count(ops, Emitter::InvalidateTextureData) + c.psBarriers, c.texInval);
    }
}

TEST(Lower, GlassFrameEmitters)
{
    const Recording r = glassFrame0();
    const Built b = plan(r, chipCaps(), JobContext{});
    const std::vector<EmitOp> ops = lower(b.graph, r, JobContext{true});
    EXPECT_EQ(count(ops, Emitter::DrawMacro), uint32_t(r.draws.size()));
    EXPECT_EQ(count(ops, Emitter::Present), 1u);
    EXPECT_EQ(ops.front().emitter, Emitter::InvalidateShaderCaches);
    EXPECT_TRUE(ops.front().arg & InvalidateInstructions) << "the heap grew: the job's entry drops instructions";
    std::set<Emitter> outside;
    for (const EmitOp &op : ops)
        if (!emitterInfo(op.emitter).inKit)
            outside.insert(op.emitter);
    EXPECT_EQ(outside, (std::set<Emitter>{Emitter::ProgramState, Emitter::Bindings, Emitter::Present}));
}

TEST(Lower, WaitInvalidatesAfterTheAcquire)
{
    Rec rec;
    const uint32_t event = rec.sampler(), target = rec.texture(80, 64, 64);
    rec.wait(event, 1);
    rec.render(pass({colour(target, Load::Clear, Store::Store)}, 64, 64));
    rec.draw(rec.state(rec.program(caState(1, {80})), {}));
    rec.end();
    rec.signal(event, 2);
    const Built b = plan(rec.r, chipCaps(), JobContext{});
    const std::vector<EmitOp> ops = lower(b.graph, rec.r, JobContext{});
    size_t acquire = 0;
    while (ops[acquire].emitter != Emitter::SemaphoreAcquire)
        ++acquire;
    EXPECT_EQ(ops[acquire + 1].emitter, Emitter::InvalidateShaderCachesNoWfi);
    EXPECT_EQ(ops[acquire + 2].emitter, Emitter::InvalidateTextureData);
    EXPECT_EQ(b.graph.nodes.back().before, TiledCacheFlush) << "the release waits for idle itself";
}

}  // namespace
}  // namespace raw::plan::test
