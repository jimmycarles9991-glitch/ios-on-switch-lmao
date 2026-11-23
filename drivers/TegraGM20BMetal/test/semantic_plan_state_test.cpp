// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../TegraGM20BMetalPlanApply.h"
#include "semantic_plan_fixture.h"
#include <map>
#include <vector>

namespace {

struct Sink {
    uint32_t bufferCalls = 0, textureCalls = 0, samplerCalls = 0, rasterCalls = 0, kernelCalls = 0;
    std::map<std::pair<uint32_t, uint32_t>, gm20b::BufferBinding> bindings;
    uint32_t cull = 0, depthClip = 0, depthClipCalls = 0;

    void buffer(uint32_t stage, uint32_t slot, const gm20b::BufferBinding &value)
    {
        ++bufferCalls;
        if (value.source == gm20b::BufferBinding::Source::Offset)
            bindings[{stage, slot}].offset = value.offset;
        else
            bindings[{stage, slot}] = value;
    }

    void texture(uint32_t, uint32_t, void *)
    {
        ++textureCalls;
    }

    void sampler(uint32_t, uint32_t, void *)
    {
        ++samplerCalls;
    }

    void raster(const gm20b::RasterState &next, const gm20b::RasterState *previous)
    {
        ++rasterCalls;
        if (gm20b::rasterChanged(next, previous, gm20b::RasterState::Cull, &gm20b::RasterState::cull))
            cull = next.cull;
        if (gm20b::rasterChanged(next, previous, gm20b::RasterState::DepthClip, &gm20b::RasterState::depthClip)) {
            depthClip = next.depthClip;
            ++depthClipCalls;
        }
    }

    void kernel(const gm20b::KernelState &, const gm20b::KernelState *)
    {
        ++kernelCalls;
    }
};

TEST(SemanticPlan, PassRasterGroupsSurviveBindingChangesAndSeparatePasses)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpPipeline, 0, 0, 42), {});
    for (uint32_t i = 0; i < 100; ++i) {
        plan.record(binding(OpBuffer, 2, 6, 90, i * 256), {});
        draw(plan);
    }
    Cmd scissor = cmd(OpScissor);
    scissor.a = 10;
    scissor.b = 20;
    scissor.c = uint64_t(40) << 32 | 30;
    plan.record(scissor, {});
    draw(plan);
    Cmd empty = cmd(OpDraw, 3);
    empty.b = 6;
    plan.record(empty, {});  // zero instances
    plan.record(cmd(OpDrawIndirect, 3), {});
    plan.record(cmd(OpEnd), {});
    const size_t second = plan.size();
    plan.record(cmd(OpRender), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    std::vector<uint32_t> counts;
    plan.forEachRasterGroup(0, [&](const auto &, uint32_t count) {
        counts.push_back(count);
    });
    EXPECT_EQ(counts, (std::vector<uint32_t>{100, 1}));
    counts.clear();
    plan.forEachRasterGroup(second, [&](const auto &, uint32_t count) {
        counts.push_back(count);
    });
    EXPECT_EQ(counts, (std::vector<uint32_t>{1}));
    plan.clear();
    plan.record(cmd(OpRender), {});
    draw(plan);
    counts.clear();
    plan.forEachRasterGroup(0, [&](const auto &, uint32_t count) {
        counts.push_back(count);
    });
    EXPECT_EQ(counts, (std::vector<uint32_t>{1}));
}

TEST(SemanticPlan, TenThousandUnchangedDrawsShareOneSnapshot)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    for (uint32_t i = 0; i < 10000; ++i) {
        plan.record(binding(OpBuffer, 1, 2, 100, 256), {});
        plan.record(binding(OpTexture, 2, 0, 200), {});
        plan.record(binding(OpSampler, 2, 0, 300), {});
        draw(plan, i);
    }
    ASSERT_EQ(plan.size(), 10001u);
    ASSERT_EQ(plan.renderStates.size(), 1u);
    Sink sink;
    gm20b::StateCursor cursor;
    for (size_t at = 1; at < plan.size(); ++at) {
        EXPECT_EQ(plan.operation(at).op, OpDraw);
        gm20b::applyRenderState(plan, plan.draw(at).state, cursor, sink);
    }
    EXPECT_EQ(sink.bufferCalls, 1u);
    EXPECT_EQ(sink.textureCalls, 1u);
    EXPECT_EQ(sink.samplerCalls, 1u);
    EXPECT_EQ(sink.rasterCalls, 1u);
}

TEST(SemanticPlan, OffsetChangePreservesTheEarlierImmutableBinding)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 4, 77, 64), {});
    draw(plan);
    plan.record(binding(OpBufferOffset, 1, 4, 0, 128), {});
    draw(plan);
    const auto &first = plan.renderStates[plan.draw(1).state];
    const auto &second = plan.renderStates[plan.draw(2).state];
    EXPECT_EQ(plan.buffers[0][first.buffers[0]][4].offset, 64u);
    EXPECT_EQ(plan.buffers[0][second.buffers[0]][4].offset, 128u);
    EXPECT_EQ(first.raster, second.raster);
    EXPECT_EQ(first.textures[1], second.textures[1]);
    Sink sink;
    gm20b::StateCursor cursor;
    gm20b::applyRenderState(plan, plan.draw(1).state, cursor, sink);
    gm20b::applyRenderState(plan, plan.draw(2).state, cursor, sink);
    EXPECT_EQ(sink.bufferCalls, 2u);
    EXPECT_EQ(sink.rasterCalls, 1u);
}

TEST(SemanticPlan, RasterAndOffsetChangesReuseResourceIdentity)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 42), {});
    plan.record(binding(OpTexture, 2, 0, 43), {});
    uint32_t builds = 0, coarse = 0, precise = 0;
    for (uint32_t i = 0; i < 10000; ++i) {
        Cmd scissor = cmd(OpScissor);
        scissor.a = i;
        plan.record(scissor, {});
        plan.record(binding(OpBufferOffset, 1, 0, 0, i * 16), {});
        draw(plan);
        const auto at = plan.size() - 1;
        if (!i)
            coarse = plan.operation(at).uses;
        EXPECT_EQ(plan.operation(at).uses, coarse);
        plan.resolveRenderUses(plan.draw(at).state, object(90), [&](std::vector<gm20b::ResourceUse> &out) {
            ++builds;
            out = std::vector<gm20b::ResourceUse>{{object(42), gm20b::ResourceUse::Read}};
        });
        if (!i)
            precise = plan.operation(at).uses;
        EXPECT_EQ(plan.operation(at).uses, precise);
    }
    EXPECT_EQ(builds, 1u);
    EXPECT_EQ(plan.renderStates.size(), 10000u);
    std::map<void *, uint8_t> uses;
    plan.prepareRange(0, plan.size());
    plan.forEachUse(0, plan.size(), [&](void *resource, uint8_t access) {
        uses[resource] = access;
    });
    EXPECT_EQ(uses, (std::map<void *, uint8_t>{{object(42), gm20b::ResourceUse::Read}}));
}

TEST(SemanticPlan, NativeResourceIdentityTracksObjectsSamplersPipelinesAndEncoderReset)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    uint32_t builds = 0;
    auto capture = [&](uintptr_t pipeline) {
        draw(plan);
        plan.resolveRenderUses(plan.draw(plan.size() - 1).state, object(pipeline),
                               [&](std::vector<gm20b::ResourceUse> &out) {
                                   ++builds;
                                   out = std::vector<gm20b::ResourceUse>{};
                               });
    };
    plan.record(binding(OpBuffer, 1, 0, 42), {});
    capture(90);
    capture(91);
    capture(90);
    EXPECT_EQ(builds, 2u);
    plan.record(binding(OpBuffer, 1, 0, 42, 128), {});
    capture(90);
    EXPECT_EQ(builds, 2u);
    plan.record(binding(OpBuffer, 1, 0, 43), {});
    capture(90);
    EXPECT_EQ(builds, 3u);
    plan.record(binding(OpSampler, 2, 0, 44), {});
    capture(90);
    EXPECT_EQ(builds, 4u);
    plan.record(binding(OpTexture, 2, 0, 45), {});
    capture(90);
    EXPECT_EQ(builds, 5u);
    Cmd bytes = cmd(OpBytes, 1, 0);
    bytes.b = 16;
    plan.record(bytes, {});
    capture(90);
    EXPECT_EQ(builds, 6u);
    bytes.a = 64;
    plan.record(bytes, {});
    capture(90);
    EXPECT_EQ(builds, 6u);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    capture(90);
    EXPECT_EQ(builds, 7u);
    plan.clear();
    plan.record(cmd(OpRender), {});
    capture(90);
    EXPECT_EQ(builds, 8u);
}

TEST(SemanticPlan, KernelResourcePlansSurviveOffsetAndDispatchChanges)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 42), {});
    uint32_t builds = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
        plan.record(binding(OpKernelBufferOffset, 3, 0, 0, i * 16), {});
        Cmd dispatch = cmd(OpDispatch);
        dispatch.c = 1 | uint64_t(1) << 16 | uint64_t(1) << 32;
        plan.record(dispatch, words({i + 1, 1, 1, i + 1, 1, 1, 0, 0, 0, 0, 0, 0}));
        plan.setKernelUses(plan.size() - 1, object(90), [&](std::vector<gm20b::ResourceUse> &out) {
            ++builds;
            out = std::vector<gm20b::ResourceUse>{{object(42), gm20b::ResourceUse::Read}};
        });
    }
    EXPECT_EQ(builds, 1u);
    plan.setKernelUses(plan.size() - 1, object(91), [&](std::vector<gm20b::ResourceUse> &out) {
        ++builds;
        out = std::vector<gm20b::ResourceUse>{{object(42), gm20b::ResourceUse::Write}};
    });
    EXPECT_EQ(builds, 2u);
    std::map<void *, uint8_t> uses;
    plan.forEachUse(plan.size() - 1, plan.size(), [&](void *resource, uint8_t access) {
        uses[resource] = access;
    });
    EXPECT_EQ(uses[object(42)], gm20b::ResourceUse::Write);
}

TEST(SemanticPlan, LargeResourceUnionPreservesAliasesAndAccess)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    std::vector<gm20b::ResourceUse> uses;
    for (uintptr_t i = 1; i <= 4096; ++i) {
        uses.push_back({object(i), gm20b::ResourceUse::Read});
        uses.push_back({object(i), gm20b::ResourceUse::Write});
    }
    plan.addUses(0, uses);
    uint32_t visited = 0;
    plan.forEachUse(0, 1, [&](void *, uint8_t access) {
        EXPECT_EQ(access, gm20b::ResourceUse::Read | gm20b::ResourceUse::Write);
        ++visited;
    });
    EXPECT_EQ(visited, 4096u);
}

TEST(SemanticPlan, ArgumentBytesKeepTheirChunkAndExtent)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd bytes = cmd(OpBytes, 2, 3);
    bytes.a = 256;
    bytes.b = 80;
    bytes.c = 2;
    plan.record(bytes, {});
    draw(plan);
    bytes.a = 512;
    bytes.b = 96;
    plan.record(bytes, {});
    draw(plan);
    const auto &first = plan.renderStates[plan.draw(1).state];
    const auto &binding = plan.buffers[1][first.buffers[1]][3];
    EXPECT_EQ(binding.source, gm20b::BufferBinding::Source::Bytes);
    EXPECT_EQ(binding.chunk, 2u);
    EXPECT_EQ(binding.offset, 256u);
    EXPECT_EQ(binding.length, 80u);
}

TEST(SemanticPlan, FloatBitPatternsRemainDistinct)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd blend = cmd(OpBlendColor);
    plan.record(blend, words({0, 0, 0, 0}));
    draw(plan);
    plan.record(blend, words({0x80000000, 0, 0, 0}));
    draw(plan);
    const auto &first = plan.renderStates[plan.draw(1).state];
    const auto &second = plan.renderStates[plan.draw(2).state];
    EXPECT_NE(first.raster, second.raster);
    EXPECT_EQ(plan.raster[first.raster].blend[0], 0u);
    EXPECT_EQ(plan.raster[second.raster].blend[0], 0x80000000u);
}

TEST(SemanticPlan, EventsKeepTheirExactSemanticPositions)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 1), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    Cmd signal = binding(OpSignalEvent, 0, 0, 900, 12);
    plan.record(signal, {});
    Cmd wait = binding(OpWaitEvent, 0, 0, 901, 13);
    wait.a = 17;
    plan.record(wait, {});
    ASSERT_EQ(plan.events().size(), 2u);
    EXPECT_EQ(plan.events()[0].position, 3u);
    EXPECT_TRUE(plan.events()[0].signal);
    EXPECT_EQ(plan.events()[1].position, 4u);
    EXPECT_EQ(plan.events()[1].timeout, 17u);
    EXPECT_EQ(plan.legacy(3).c, 12u);
    EXPECT_EQ(plan.encoderEnd(0), 3u);
}

// A depth clip mode is its encoder's: the next encoder names none (the raw encoder starts each at Metal's Clip), and one
// set again in a later encoder is applied though it equals the first's
TEST(SemanticPlan, ADepthClipModeIsItsEncodersOnly)
{
    SwitchMetalCommandPlan plan;
    Cmd clamp = cmd(OpDepthClip);
    clamp.a = 1;
    plan.record(cmd(OpRender), {});
    plan.record(clamp, {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    plan.record(clamp, {});
    draw(plan);
    Sink sink;
    gm20b::StateCursor cursor;
    gm20b::applyRenderState(plan, plan.draw(1).state, cursor, sink);
    EXPECT_EQ(sink.depthClip, 1u);
    EXPECT_EQ(sink.depthClipCalls, 1u);
    gm20b::applyRenderState(plan, plan.draw(4).state, cursor, sink);
    EXPECT_EQ(sink.depthClipCalls, 1u);
    EXPECT_FALSE(plan.raster[plan.renderStates[plan.draw(4).state].raster].set & gm20b::RasterState::DepthClip);
    gm20b::applyRenderState(plan, plan.draw(7).state, cursor, sink);
    EXPECT_EQ(sink.depthClipCalls, 2u);
    EXPECT_EQ(sink.depthClip, 1u);
}

TEST(SemanticPlan, NewEncodersStartWithIndependentState)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 20), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    draw(plan);
    const auto &state = plan.renderStates[plan.draw(4).state];
    EXPECT_EQ(plan.buffers[0][state.buffers[0]][0].source, gm20b::BufferBinding::Source::Unset);
    EXPECT_EQ(plan.encoderEnd(0), 3u);
}

TEST(SemanticPlan, KernelDispatchHasTypedGridAndSharedState)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 99, 16), {});
    plan.record(binding(OpKernelTexture, 3, 2, 100), {});
    Cmd dispatch = cmd(OpDispatch, 3, 1);
    dispatch.a = 1u << 2;
    dispatch.c = 8 | uint64_t(4) << 16 | uint64_t(2) << 32;
    dispatch.obj2 = object(999);
    plan.record(dispatch, words({16, 12, 8, 2, 3, 4, 0, 0, 0, 0, 0, 0}));
    plan.record(dispatch, words({24, 12, 8, 3, 3, 4, 0, 0, 0, 0, 0, 0}));
    ASSERT_EQ(plan.kernelStates.size(), 1u);
    EXPECT_EQ(plan.dispatch(1).groups, (std::array<uint64_t, 3>{2, 3, 4}));
    EXPECT_EQ(plan.dispatch(2).grid, (std::array<uint64_t, 3>{24, 12, 8}));
    EXPECT_EQ(plan.dispatch(1).threads, (std::array<uint32_t, 3>{8, 4, 2}));
    EXPECT_TRUE(plan.dispatch(1).perThread);
    EXPECT_EQ(plan.dispatch(1).surfaceSlots, 1u << 2);
    EXPECT_EQ(plan.legacy(1).a, 1u << 2);
    Sink sink;
    gm20b::StateCursor cursor;
    gm20b::applyKernelState(plan, plan.dispatch(1).state, cursor, sink);
    gm20b::applyKernelState(plan, plan.dispatch(2).state, cursor, sink);
    EXPECT_EQ(sink.bufferCalls, 1u);
    EXPECT_EQ(sink.textureCalls, 1u);
}

}  // namespace
