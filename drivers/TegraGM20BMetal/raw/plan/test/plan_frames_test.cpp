// SPDX-License-Identifier: MIT
// Golden graphs of recorded frames: their nodes, and the barriers the edge rules place against today's unconditional
// prologue. The glass frames are Core Animation's as a Mac recorded them with tile shaders denied (the Apple3 path); the
// scroll frame's composition is assumed from pacing.list's measured shape.
#include "../plan_validate.h"
#include "fixtures.h"
#include "today_model.h"
#include <gtest/gtest.h>
#include <cstdio>

namespace raw::plan::test {
namespace {

void print(const char *name, const Counts &c)
{
    std::printf("%-26s drains %2u (switch %u)  wfi %2u  shinv %2u  shinv_nowfi %u  texinv %2u  ps %u  tiled %2u\n",
                name, c.drains, c.switchDrains, c.wfi, c.shaderInval, c.shaderInvalNoWfi, c.texInval, c.psBarriers,
                c.tiledFlushes);
}

// drains, WFI, shader-cache invalidates that wait, texture invalidates (pixel-ordered ones included), tiled-cache flushes
struct Expected {
    uint32_t drains, wfi, shinv, texinv, tiled;
};

void expectCounts(const Counts &c, const Expected &e, const char *what)
{
    EXPECT_EQ(c.drains, e.drains) << what;
    EXPECT_EQ(c.wfi, e.wfi) << what;
    EXPECT_EQ(c.shaderInval, e.shinv) << what;
    EXPECT_EQ(c.texInval, e.texinv) << what;
    EXPECT_EQ(c.tiledFlushes, e.tiled) << what;
}

struct Frame {
    Built built;
    Counts edges, relaxed, today;
};

Frame planFrame(const Recording &r, const char *name)
{
    Frame f;
    f.built = plan(r, chipCaps(), JobContext{});
    EXPECT_TRUE(validate(r, f.built, Answers{}, ChannelState{}).plannable()) << name;
    f.edges = countGraph(f.built.graph);
    const uint32_t barrierProbes = 1u << uint32_t(Probe::PassPixelBarrier) | 1u << uint32_t(Probe::IdleJobBoundary) |
                                   1u << uint32_t(Probe::SwitchDrain);
    f.relaxed = countGraph(plan(r, chipCaps(barrierProbes), JobContext{}).graph);
    f.today = todayCounts(f.built.graph, r);
    std::printf("%s\n", name);
    print("  today (unconditional)", f.today);
    print("  edges (selectable)", f.edges);
    print("  edges + barrier probes", f.relaxed);
    std::printf("%s", describe(f.built.graph).c_str());
    return f;
}

TEST(Frames, GlassFirstFrame)
{
    const Recording r = glassFrame0();
    const Frame f = planFrame(r, "glass frame 0");
    const WorkGraph &g = f.built.graph;
    ASSERT_EQ(g.nodes.size(), 7u);
    for (uint32_t i = 0; i < 6; ++i)
        EXPECT_EQ(g.nodes[i].kind, NodeKind::RenderPass);
    EXPECT_EQ(g.nodes[6].kind, NodeKind::Present);
    EXPECT_EQ(g.nodes[0].before, 0) << "the content pass depends on nothing in this command buffer";
    for (uint32_t i = 1; i < 5; ++i)
        EXPECT_EQ(g.nodes[i].before, TiledCacheFlush | WaitForIdle | InvalidateTextureData) << i;
    EXPECT_EQ(g.nodes[5].before, TiledCacheFlush | WaitForIdle | InvalidateTextureData | InvalidateShaderData)
        << "the glass pass samples the pyramid and its interlock reads the drawable as storage";
    EXPECT_EQ(g.runs[g.nodes[5].runs.first + 1].before,
              TiledCacheFlush | FlushShaderData | InvalidateShaderData | InvalidateTextureData)
        << "the merge's storage access meets the background draws' colour writes";
    EXPECT_EQ(g.nodes[6].before, TiledCacheFlush | FlushShaderData) << "the present reads what the interlock stored";
    expectCounts(f.today, {9, 6, 9, 8, 7}, "today");
    expectCounts(f.edges, {8, 4, 4, 7, 7},
                 "edges: the job's entry, the capture-pyramid-glass chain, the merge, the present");
    expectCounts(f.relaxed, {3, 0, 3, 7, 7}, "edges and the barrier probes");
}

TEST(Frames, GlassSteadyFrame)
{
    const Recording r = glassFrame1();
    const Frame f = planFrame(r, "glass frame 1");
    expectCounts(f.today, {4, 1, 4, 3, 2}, "today");
    expectCounts(f.edges, {3, 0, 3, 2, 2}, "edges: the job's entry, the merge's storage barrier, the present");
    expectCounts(f.relaxed, {2, 0, 2, 2, 2}, "edges and the barrier probes");
}

TEST(Frames, Scroll)
{
    const Frame f = planFrame(scrollFrame(true), "scroll frame (synthetic, memoryless aliased per tag)");
    expectCounts(f.today, {13, 12, 13, 13, 12}, "today");
    expectCounts(f.edges, {12, 11, 1, 5, 12}, "edges: every pass aliases the memoryless backing of the one before");
    expectCounts(f.relaxed, {0, 0, 0, 12, 12}, "edges and the barrier probes");
    const Frame own = planFrame(scrollFrame(false), "scroll frame (synthetic, a memoryless backing per pass)");
    expectCounts(own.today, {13, 12, 13, 13, 12}, "today, a backing per pass");
    expectCounts(own.edges, {5, 4, 1, 5, 5}, "edges, a backing per pass");
    expectCounts(own.relaxed, {0, 0, 0, 5, 5}, "edges and the barrier probes, a backing per pass");
}

TEST(Frames, MixedEngines)
{
    const Frame f = planFrame(mixedFrame(), "mixed frame (synthetic: copy, fill, dispatch, resolve, mips, indirect)");
    expectCounts(f.today, {10, 9, 7, 6, 4}, "today");
    expectCounts(f.edges, {8, 3, 3, 6, 4}, "edges");
    expectCounts(f.relaxed, {6, 0, 1, 7, 4}, "edges and the barrier probes");
    EXPECT_EQ(f.edges.switchDrains, 2u);
    EXPECT_EQ(f.edges.setReference, 1u) << "the indirect draw reads arguments the GPU wrote";
}

}  // namespace
}  // namespace raw::plan::test
