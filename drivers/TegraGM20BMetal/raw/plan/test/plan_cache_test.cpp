// SPDX-License-Identifier: MIT
// The plan cache: a repeated frame structure is planned once and costs a key and a lookup after that; parameters and the
// drawable's identity do not change the key, structure does.
#include "../plan_cache.h"
#include "../plan_key.h"
#include "fixtures.h"
#include <gtest/gtest.h>

namespace raw::plan::test {
namespace {

TEST(Cache, RepeatedStructureIsALookup)
{
    PlanCache cache;
    const Caps caps = chipCaps();
    const Plan *first = &cache.get(glassFrame1(), caps, Answers{});
    for (int frame = 0; frame < 60; ++frame) {
        Recording r = glassFrame1();
        for (Draw &d : r.draws)
            d.count += uint32_t(frame);
        EXPECT_EQ(&cache.get(r, caps, Answers{}), first);
    }
    EXPECT_EQ(cache.plans(), 1u);
    EXPECT_EQ(cache.hits(), 60u);
    EXPECT_TRUE(first->finding.plannable());
    EXPECT_FALSE(first->ops.empty());
}

TEST(Cache, AnotherDrawableOfTheSameShapeHits)
{
    Recording a = glassFrame1(), b = glassFrame1();
    b.resources[0].handle = {77, 9};
    b.resources[0].memory += 1000;
    EXPECT_EQ(structuralKey(a, chipCaps()), structuralKey(b, chipCaps()));
}

TEST(Cache, StructureMisses)
{
    PlanCache cache;
    const Caps caps = chipCaps();
    cache.get(glassFrame0(), caps, Answers{});
    cache.get(glassFrame1(), caps, Answers{});
    Recording stale = glassFrame1();
    stale.resources[0].flags |= ResourceFacts::ImageStale;
    cache.get(stale, caps, Answers{});
    cache.get(glassFrame1(), chipCaps(1u << uint32_t(Probe::PassPixelBarrier)), Answers{});
    EXPECT_EQ(cache.plans(), 4u);
    EXPECT_EQ(cache.hits(), 0u);
}

TEST(Cache, AnOutOfRangeCopyIsAnotherStructure)
{
    const Caps caps = chipCaps();
    Recording r = mixedFrame();
    PlanCache cache;
    EXPECT_TRUE(cache.get(r, caps, Answers{}).finding.plannable());
    r.transfers[1].destinationOffset = 250;
    const Plan &bad = cache.get(r, caps, Answers{});
    EXPECT_EQ(bad.finding.problem, Problem::BufferRange);
    EXPECT_EQ(cache.plans(), 2u);
}

TEST(Cache, NodeKeysFollowStructure)
{
    const Built a = plan(glassFrame0(), chipCaps(), JobContext{});
    const Built b = plan(glassFrame1(), chipCaps(), JobContext{});
    EXPECT_NE(a.graph.nodes[0].key, 0u);
    EXPECT_NE(a.graph.nodes[1].key, a.graph.nodes[2].key) << "different targets and states";
    EXPECT_NE(a.graph.nodes[0].key, b.graph.nodes[0].key) << "the steady frame's pass also draws the glass";
}

// Nodes that lower to different words have different keys: a Clear load emits a clear, a DontCare load does not
TEST(Cache, NodeKeysFollowWhatLoweringReads)
{
    const auto keyOf = [](Load load, uint8_t indexSize) {
        Rec rec;
        const uint32_t target = rec.texture(80, 64, 64), indices = rec.buffer(256);
        rec.render(pass({colour(target, load, Store::Store)}, 64, 64));
        rec.draw(rec.state(rec.program(caState(1, {80})), {}), 6, indexSize, indexSize ? indices : kNone);
        rec.end();
        return plan(rec.r, chipCaps(), JobContext{}).graph.nodes[0].key;
    };
    EXPECT_NE(keyOf(Load::Clear, 2), keyOf(Load::DontCare, 2));
    EXPECT_NE(keyOf(Load::Clear, 2), keyOf(Load::Clear, 0));
    EXPECT_EQ(keyOf(Load::Clear, 2), keyOf(Load::Clear, 2));
}

TEST(Cache, LeastRecentlyUsedGoesFirst)
{
    PlanCache cache(2);
    const Caps caps = chipCaps();
    cache.get(glassFrame0(), caps, Answers{});
    cache.get(glassFrame1(), caps, Answers{});
    cache.get(glassFrame0(), caps, Answers{});
    cache.get(scrollFrame(), caps, Answers{});
    EXPECT_EQ(cache.size(), 2u);
    cache.get(glassFrame0(), caps, Answers{});
    EXPECT_EQ(cache.hits(), 2u);
    cache.get(glassFrame1(), caps, Answers{});
    EXPECT_EQ(cache.plans(), 4u);
}

}  // namespace
}  // namespace raw::plan::test
