// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/RawMetalPlanApply.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

void *object(uintptr_t value)
{
    return reinterpret_cast<void *>(value);
}

Cmd binding(Op op, uint8_t stage, uint16_t slot, uintptr_t value, uint64_t offset = 0)
{
    Cmd c = cmd(op, stage, slot);
    c.obj = object(value);
    c.c = offset;
    return c;
}

void draw(RMCommandPlan &plan, uint32_t first = 0)
{
    Cmd c = cmd(OpDraw, 3);
    c.a = first;
    c.b = 6;
    c.c = uint64_t(1) << 32;
    plan.record(c, {});
}

struct Sink {
    uint32_t bufferCalls = 0, textureCalls = 0, samplerCalls = 0, rasterCalls = 0, kernelCalls = 0;
    std::map<std::pair<uint32_t, uint32_t>, raw::BufferBinding> bindings;
    uint32_t cull = 0, depthClip = 0, depthClipCalls = 0;

    void buffer(uint32_t stage, uint32_t slot, const raw::BufferBinding &value)
    {
        ++bufferCalls;
        if (value.source == raw::BufferBinding::Source::Offset)
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

    void raster(const raw::RasterState &next, const raw::RasterState *previous)
    {
        ++rasterCalls;
        if (raw::rasterChanged(next, previous, raw::RasterState::Cull, &raw::RasterState::cull))
            cull = next.cull;
        if (raw::rasterChanged(next, previous, raw::RasterState::DepthClip, &raw::RasterState::depthClip)) {
            depthClip = next.depthClip;
            ++depthClipCalls;
        }
    }

    void kernel(const raw::KernelState &, const raw::KernelState *)
    {
        ++kernelCalls;
    }
};

// A command buffer's own plan, which the recording pool's replaces, allocates nothing; one recorded into as made (the
// pool had none) reads its operations of no resource as using none
TEST(SemanticPlan, AConstructedPlanHoldsNoStorageAndItsFirstOperationsUseNothing)
{
    RMCommandPlan plan;
    EXPECT_EQ(plan.storageBytes(), 0u);
    plan.record(cmd(OpRender), {});
    plan.record(cmd(OpEnd), {});
    size_t uses = 0;
    plan.forEachUse(0, plan.size(), [&](void *, uint8_t) {
        ++uses;
    });
    EXPECT_EQ(uses, 0u);
}

TEST(SemanticPlan, VisibilityModesOnAPassWithAResultBufferAreOperationsInOrder)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd untracked = cmd(OpVisibility);
    plan.record(untracked, {});
    const size_t first = plan.size();
    Cmd counting = cmd(OpVisibility);
    counting.a = 2;
    counting.obj = object(0x40);
    plan.record(counting, {});
    draw(plan);
    counting.c = 8;
    plan.record(counting, {});
    Cmd disabled = cmd(OpVisibility);
    disabled.obj = object(0x40);
    plan.record(disabled, {});
    plan.record(cmd(OpEnd), {});
    std::vector<Cmd> modes;
    for (size_t at = 0; at < plan.size(); ++at)
        if (plan.operation(at).op == OpVisibility)
            modes.push_back(plan.legacy(at));
    ASSERT_EQ(modes.size(), 3u) << "a Disabled with no result buffer (Core Animation's) stays a setter";
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(modes[0].a, 2u);
    EXPECT_EQ(modes[0].c, 0u);
    EXPECT_EQ(modes[1].c, 8u);
    EXPECT_EQ(modes[2].a, 0u);
    for (const Cmd &m : modes)
        EXPECT_EQ(m.obj, object(0x40));
}

TEST(SemanticPlan, PredicationRecordsKeepTheirWordCompareAndReference)
{
    RMCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    Cmd start = cmd(OpPredicate, 1);
    start.obj = object(0x80);
    start.c = 0x124;
    start.a = 4;
    start.b = 0xfffffffeu;
    plan.record(start, {});
    plan.record(cmd(OpPredicate, 3), {});
    plan.record(cmd(OpEnd), {});
    ASSERT_EQ(plan.size(), 4u);
    const Cmd got = plan.legacy(1);
    EXPECT_EQ(got.op, OpPredicate);
    EXPECT_EQ(got.stage, 1);
    EXPECT_EQ(got.obj, object(0x80));
    EXPECT_EQ(got.c, 0x124u);
    EXPECT_EQ(got.a, 4u);
    EXPECT_EQ(got.b, 0xfffffffeu);
    EXPECT_EQ(plan.legacy(2).stage, 3);
}

TEST(SemanticPlan, SettersCollapseBeforeTheFirstDraw)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    for (uint64_t i = 0; i < 1000; ++i)
        EXPECT_FALSE(plan.record(binding(OpBuffer, 1, 0, 42, i * 16), {}));
    EXPECT_EQ(plan.size(), 1u);
    EXPECT_TRUE(plan.renderStates.empty());
    draw(plan);
    ASSERT_EQ(plan.size(), 2u);
    const auto &state = plan.renderStates[plan.draw(1).state];
    EXPECT_EQ(plan.buffers[0][state.buffers[0]][0].offset, 999u * 16);
    EXPECT_EQ(plan.buffers[0].size(), 1u);
}

Cmd bytes(uint8_t stage, uint16_t slot, uint64_t offset)
{
    Cmd c = cmd(OpBytes, stage, slot);
    c.a = offset;
    c.b = 64;
    return c;
}

TEST(SemanticPlan, ADrawsSetBytesCaptureCopiesOnlyTheChunkOfSlotsTheyEdited)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 20, 7, 4096), {});
    constexpr uint32_t kDraws = 200;
    for (uint32_t d = 0; d < kDraws; ++d) {
        for (uint16_t slot = 0; slot < 4; ++slot)
            plan.record(bytes(1, slot, uint64_t(d) * 1024 + slot * 256), {});
        draw(plan);
    }
    ASSERT_EQ(plan.buffers[0].size(), kDraws);
    for (uint32_t d = 0; d < kDraws; ++d) {
        const auto &state = plan.renderStates[plan.draw(1 + d).state];
        const auto bindings = plan.buffers[0][state.buffers[0]];
        for (uint16_t slot = 0; slot < 4; ++slot) {
            EXPECT_EQ(bindings[slot].source, raw::BufferBinding::Source::Bytes);
            EXPECT_EQ(bindings[slot].offset, uint64_t(d) * 1024 + slot * 256);
        }
        EXPECT_EQ(bindings[20].object, object(7));
        EXPECT_EQ(bindings[20].offset, 4096u);
        EXPECT_EQ(bindings[5].source, raw::BufferBinding::Source::Unset);
        uint32_t set = 0;
        for (const raw::BufferBinding &b : bindings)
            set += b.source != raw::BufferBinding::Source::Unset;
        EXPECT_EQ(set, 5u);
    }
    // the first capture's eight chunks, then one a draw: far below a whole table a draw
    EXPECT_LT(plan.buffers[0].storageBytes(), kDraws * sizeof(raw::BufferBindings) / 3);
}

TEST(SemanticPlan, ADrawsTextureCaptureCopiesOnlyTheChunkOfSlotsItBound)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpTexture, 2, 9, 70), {});
    plan.record(binding(OpTexture, 2, 1, 0), {});
    constexpr uint32_t kDraws = 200;
    for (uint32_t d = 0; d < kDraws; ++d) {
        plan.record(binding(OpTexture, 2, 0, 100 + d % 3), {});
        draw(plan);
    }
    for (uint32_t d = 0; d < kDraws; ++d) {
        const auto &state = plan.renderStates[plan.draw(1 + d).state];
        const auto textures = plan.textures[1][state.textures[1]];
        EXPECT_EQ(textures.objects[0], object(100 + d % 3));
        EXPECT_EQ(textures.objects[1], nullptr);
        EXPECT_EQ(textures.objects[9], object(70));
        EXPECT_EQ(textures.set, 1u << 0 | 1u << 1 | 1u << 9);
    }
    // a capture's chunk of four slots and its chunk list, half of a whole table's copy and its mask at most
    EXPECT_LT(plan.textures[1].storageBytes(), kDraws * 32 * sizeof(void *) / 2);
}

TEST(SemanticPlan, AWordKeyedMapKeepsItsStorageAcrossClears)
{
    raw::StampMap<std::tuple<uint32_t, uintptr_t, uintptr_t>, uint32_t, raw::WordsHash> map;
    const auto key = [](uint32_t i) {
        return std::make_tuple(i, uintptr_t(i) * 64, uintptr_t(i & 3));
    };
    for (uint32_t i = 0; i < 100; ++i)
        EXPECT_TRUE(map.insert(key(i), i).second);
    const size_t bytes = map.storageBytes();
    for (uint32_t round = 0; round < 50; ++round) {
        map.clear();
        EXPECT_EQ(map.find(key(1)), nullptr);
        for (uint32_t i = 0; i < 100; ++i)
            EXPECT_TRUE(map.insert(key(i), i + round).second);
        EXPECT_FALSE(map.insert(key(5), 0).second);
        EXPECT_EQ(*map.find(key(50)), 50 + round);
        EXPECT_EQ(map.find(std::make_tuple(50u, uintptr_t(50) * 64, uintptr_t(3))), nullptr);
    }
    EXPECT_EQ(map.storageBytes(), bytes);
}

TEST(SemanticPlan, ANewEncodersFirstCaptureEmptiesOnlyTheSlotsTheLastOneSet)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(bytes(1, 3, 256), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    plan.record(bytes(1, 30, 512), {});
    draw(plan);
    const auto &state = plan.renderStates[plan.draw(plan.size() - 1).state];
    const auto bindings = plan.buffers[0][state.buffers[0]];
    EXPECT_EQ(bindings[3].source, raw::BufferBinding::Source::Unset);
    EXPECT_EQ(bindings[30].offset, 512u);
    EXPECT_EQ(bindings[30].source, raw::BufferBinding::Source::Bytes);
}

TEST(SemanticPlan, APointerMapKeepsItsStorageAcrossClears)
{
    raw::PointerMap<uint32_t> map;
    for (uintptr_t i = 1; i <= 100; ++i)
        EXPECT_TRUE(map.insert(object(i * 64), uint32_t(i)).second);
    const size_t bytes = map.storageBytes();
    for (unsigned round = 0; round < 50; ++round) {
        map.clear();
        EXPECT_EQ(map.find(object(64)), nullptr);
        for (uintptr_t i = 1; i <= 100; ++i)
            EXPECT_TRUE(map.insert(object(i * 64), uint32_t(i + round)).second);
        EXPECT_FALSE(map.insert(object(64), 0).second);
        EXPECT_EQ(*map.find(object(64 * 50)), 50 + round);
    }
    EXPECT_EQ(map.size(), 100u);
    EXPECT_EQ(map.storageBytes(), bytes);
}

TEST(SemanticPlan, PassRasterGroupsSurviveBindingChangesAndSeparatePasses)
{
    RMCommandPlan plan;
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
    RMCommandPlan plan;
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
    raw::StateCursor cursor;
    for (size_t at = 1; at < plan.size(); ++at) {
        EXPECT_EQ(plan.operation(at).op, OpDraw);
        raw::applyRenderState(plan, plan.draw(at).state, cursor, sink);
    }
    EXPECT_EQ(sink.bufferCalls, 1u);
    EXPECT_EQ(sink.textureCalls, 1u);
    EXPECT_EQ(sink.samplerCalls, 1u);
    EXPECT_EQ(sink.rasterCalls, 1u);
}

TEST(SemanticPlan, OffsetChangePreservesTheEarlierImmutableBinding)
{
    RMCommandPlan plan;
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
    raw::StateCursor cursor;
    raw::applyRenderState(plan, plan.draw(1).state, cursor, sink);
    raw::applyRenderState(plan, plan.draw(2).state, cursor, sink);
    EXPECT_EQ(sink.bufferCalls, 2u);
    EXPECT_EQ(sink.rasterCalls, 1u);
}

TEST(SemanticPlan, RasterAndOffsetChangesReuseResourceIdentity)
{
    RMCommandPlan plan;
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
        plan.resolveRenderUses(plan.draw(at).state, object(90), [&](std::vector<raw::ResourceUse> &out) {
            ++builds;
            out = std::vector<raw::ResourceUse>{{object(42), raw::ResourceUse::Read}};
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
    EXPECT_EQ(uses, (std::map<void *, uint8_t>{{object(42), raw::ResourceUse::Read}}));
}

TEST(SemanticPlan, NativeResourceIdentityTracksObjectsSamplersPipelinesAndEncoderReset)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    uint32_t builds = 0;
    auto capture = [&](uintptr_t pipeline) {
        draw(plan);
        plan.resolveRenderUses(plan.draw(plan.size() - 1).state, object(pipeline),
                               [&](std::vector<raw::ResourceUse> &out) {
                                   ++builds;
                                   out = std::vector<raw::ResourceUse>{};
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
    plan.record(cmd(OpInvocation), {});
    capture(90);
    EXPECT_EQ(builds, 7u);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    capture(90);
    EXPECT_EQ(builds, 8u);
    plan.clear();
    plan.record(cmd(OpRender), {});
    capture(90);
    EXPECT_EQ(builds, 9u);
}

TEST(SemanticPlan, KernelResourcePlansSurviveOffsetAndDispatchChanges)
{
    RMCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 42), {});
    uint32_t builds = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
        plan.record(binding(OpKernelBufferOffset, 3, 0, 0, i * 16), {});
        Cmd dispatch = cmd(OpDispatch);
        dispatch.c = 1 | uint64_t(1) << 16 | uint64_t(1) << 32;
        plan.record(dispatch, {i + 1, 1, 1, i + 1, 1, 1, 0, 0, 0, 0, 0, 0});
        plan.setKernelUses(plan.size() - 1, object(90), [&](std::vector<raw::ResourceUse> &out) {
            ++builds;
            out = std::vector<raw::ResourceUse>{{object(42), raw::ResourceUse::Read}};
        });
    }
    EXPECT_EQ(builds, 1u);
    plan.setKernelUses(plan.size() - 1, object(91), [&](std::vector<raw::ResourceUse> &out) {
        ++builds;
        out = std::vector<raw::ResourceUse>{{object(42), raw::ResourceUse::Write}};
    });
    EXPECT_EQ(builds, 2u);
    std::map<void *, uint8_t> uses;
    plan.forEachUse(plan.size() - 1, plan.size(), [&](void *resource, uint8_t access) {
        uses[resource] = access;
    });
    EXPECT_EQ(uses[object(42)], raw::ResourceUse::Write);
}

TEST(SemanticPlan, LargeResourceUnionPreservesAliasesAndAccess)
{
    RMCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    std::vector<raw::ResourceUse> uses;
    for (uintptr_t i = 1; i <= 4096; ++i) {
        uses.push_back({object(i), raw::ResourceUse::Read});
        uses.push_back({object(i), raw::ResourceUse::Write});
    }
    plan.addUses(0, uses);
    uint32_t visited = 0;
    plan.forEachUse(0, 1, [&](void *, uint8_t access) {
        EXPECT_EQ(access, raw::ResourceUse::Read | raw::ResourceUse::Write);
        ++visited;
    });
    EXPECT_EQ(visited, 4096u);
}

TEST(SemanticPlan, ArgumentBytesKeepTheirChunkAndExtent)
{
    RMCommandPlan plan;
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
    EXPECT_EQ(binding.source, raw::BufferBinding::Source::Bytes);
    EXPECT_EQ(binding.chunk, 2u);
    EXPECT_EQ(binding.offset, 256u);
    EXPECT_EQ(binding.length, 80u);
}

TEST(SemanticPlan, FloatBitPatternsRemainDistinct)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd blend = cmd(OpBlendColor);
    plan.record(blend, {0, 0, 0, 0});
    draw(plan);
    plan.record(blend, {0x80000000, 0, 0, 0});
    draw(plan);
    const auto &first = plan.renderStates[plan.draw(1).state];
    const auto &second = plan.renderStates[plan.draw(2).state];
    EXPECT_NE(first.raster, second.raster);
    EXPECT_EQ(plan.raster[first.raster].blend[0], 0u);
    EXPECT_EQ(plan.raster[second.raster].blend[0], 0x80000000u);
}

TEST(SemanticPlan, EventsKeepTheirExactSemanticPositions)
{
    RMCommandPlan plan;
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

TEST(SemanticPlan, OpaqueCallsPreservePositionAndUnknownState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd cull = cmd(OpCull);
    cull.a = 1;
    plan.record(cull, {});
    plan.record(binding(OpBuffer, 1, 0, 10), {});
    draw(plan);
    plan.record(binding(OpInvocation, 0, 0, 500), {});
    plan.record(binding(OpBufferOffset, 1, 0, 0, 128), {});
    draw(plan);
    ASSERT_EQ(plan.size(), 4u);
    const auto &call = plan.control(2);
    EXPECT_EQ(call.render, plan.draw(1).state);
    Sink sink;
    raw::StateCursor cursor;
    raw::applyRenderState(plan, call.render, cursor, sink);
    sink.cull = 2;
    sink.bindings[{1, 0}].object = object(11);
    raw::applyRenderState(plan, plan.draw(3).state, cursor, sink);
    EXPECT_EQ(sink.cull, 2u);
    EXPECT_EQ(sink.bindings[std::make_pair(1u, 0u)].object, object(11));
    EXPECT_EQ(sink.bindings[std::make_pair(1u, 0u)].offset, 128u);
}

TEST(SemanticPlan, ExplicitStateAfterOpaqueCallOverridesUnknownState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd cull = cmd(OpCull);
    cull.a = 1;
    plan.record(cull, {});
    plan.record(binding(OpInvocation, 0, 0, 500), {});
    plan.record(cull, {});
    draw(plan);
    Sink sink;
    raw::StateCursor cursor;
    raw::applyRenderState(plan, plan.control(1).render, cursor, sink);
    sink.cull = 2;
    raw::applyRenderState(plan, plan.draw(2).state, cursor, sink);
    EXPECT_EQ(sink.cull, 1u);
}

// A depth clip mode is its encoder's: the next encoder names none (the raw encoder starts each at Metal's Clip), and one
// set again in a later encoder is applied though it equals the first's
TEST(SemanticPlan, ADepthClipModeIsItsEncodersOnly)
{
    RMCommandPlan plan;
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
    raw::StateCursor cursor;
    raw::applyRenderState(plan, plan.draw(1).state, cursor, sink);
    EXPECT_EQ(sink.depthClip, 1u);
    EXPECT_EQ(sink.depthClipCalls, 1u);
    raw::applyRenderState(plan, plan.draw(4).state, cursor, sink);
    EXPECT_EQ(sink.depthClipCalls, 1u);
    EXPECT_FALSE(plan.raster[plan.renderStates[plan.draw(4).state].raster].set & raw::RasterState::DepthClip);
    raw::applyRenderState(plan, plan.draw(7).state, cursor, sink);
    EXPECT_EQ(sink.depthClipCalls, 2u);
    EXPECT_EQ(sink.depthClip, 1u);
}

TEST(SemanticPlan, NewEncodersStartWithIndependentState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 20), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    draw(plan);
    const auto &state = plan.renderStates[plan.draw(4).state];
    EXPECT_EQ(plan.buffers[0][state.buffers[0]][0].source, raw::BufferBinding::Source::Unset);
    EXPECT_EQ(plan.encoderEnd(0), 3u);
}

TEST(SemanticPlan, KernelDispatchHasTypedGridAndSharedState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 99, 16), {});
    plan.record(binding(OpKernelTexture, 3, 2, 100), {});
    Cmd dispatch = cmd(OpDispatch, 3, 1);
    dispatch.a = 1u << 2;
    dispatch.c = 8 | uint64_t(4) << 16 | uint64_t(2) << 32;
    dispatch.obj2 = object(999);
    plan.record(dispatch, {16, 12, 8, 2, 3, 4, 0, 0, 0, 0, 0, 0});
    plan.record(dispatch, {24, 12, 8, 3, 3, 4, 0, 0, 0, 0, 0, 0});
    ASSERT_EQ(plan.kernelStates.size(), 1u);
    EXPECT_EQ(plan.dispatch(1).groups, (std::array<uint64_t, 3>{2, 3, 4}));
    EXPECT_EQ(plan.dispatch(2).grid, (std::array<uint64_t, 3>{24, 12, 8}));
    EXPECT_EQ(plan.dispatch(1).threads, (std::array<uint32_t, 3>{8, 4, 2}));
    EXPECT_TRUE(plan.dispatch(1).perThread);
    EXPECT_EQ(plan.dispatch(1).surfaceSlots, 1u << 2);
    EXPECT_EQ(plan.legacy(1).a, 1u << 2);
    Sink sink;
    raw::StateCursor cursor;
    raw::applyKernelState(plan, plan.dispatch(1).state, cursor, sink);
    raw::applyKernelState(plan, plan.dispatch(2).state, cursor, sink);
    EXPECT_EQ(sink.bufferCalls, 1u);
    EXPECT_EQ(sink.textureCalls, 1u);
}

TEST(SemanticPlan, CachedRangeSummariesUnionAccessAndExcludeOtherRanges)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.addUses(0, {{object(1), raw::ResourceUse::Write}});
    plan.record(binding(OpBuffer, 1, 0, 2), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 2), {});
    Cmd dispatch = cmd(OpDispatch);
    dispatch.c = 1 | uint64_t(1) << 16 | uint64_t(1) << 32;
    plan.record(dispatch, {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, 3);
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, 3, [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses.size(), 2u);
    EXPECT_EQ(uses[object(2)], raw::ResourceUse::Read);
    uses.clear();
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(2)], raw::ResourceUse::Read | raw::ResourceUse::Write);
    std::vector<void *> writes;
    plan.forEachWritten(0, 3, [&](void *object) {
        writes.push_back(object);
    });
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0], object(1));
}

TEST(SemanticPlan, ReflectedUsesReplaceUnusedBindingResources)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpTexture, 2, 0, 1), {});
    plan.record(binding(OpTexture, 2, 1, 2), {});
    draw(plan);
    plan.setRenderUses(plan.draw(1).state, {{object(1), raw::ResourceUse::Read}});
    draw(plan);
    plan.prepareRange(0, plan.size());
    std::vector<void *> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t) {
        uses.push_back(object);
    });
    ASSERT_EQ(uses.size(), 1u);
    EXPECT_EQ(uses[0], object(1));
}

TEST(SemanticPlan, IndexedArgumentsPreserveSignedBaseVertex)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd indexed = binding(OpDrawIndexed, 3, 0, 21, 256);
    indexed.a = 6;
    indexed.b = 1;
    plan.record(indexed, {4, uint64_t(int64_t(-7)), 9});
    const auto &draw = plan.draw(1);
    EXPECT_EQ(draw.baseVertex, -7);
    EXPECT_EQ(draw.baseInstance, 9u);
    EXPECT_EQ(draw.instances, 4u);
    EXPECT_EQ(draw.indexOffset, 256u);
}

TEST(SemanticPlan, IndirectArgumentsAreReferencesNeverCpuReads)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd indirect = cmd(OpDrawIndexedIndirect, 3);
    indirect.obj = object(1);
    indirect.obj2 = object(3);
    indirect.a = 1;
    indirect.c = 64;
    indirect.x = 128;
    plan.record(indirect, {});
    const auto &draw = plan.draw(1);
    EXPECT_EQ(draw.arguments, object(3));
    EXPECT_EQ(draw.indices, object(1));
    EXPECT_EQ(draw.argumentOffset, 128u);
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(1)], raw::ResourceUse::Read);
    EXPECT_EQ(uses[object(3)], raw::ResourceUse::Read);
}

TEST(SemanticPlan, RecycledStorageDropsAllSemanticState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 10), {});
    draw(plan);
    plan.prepareRange(0, plan.size());
    plan.clear();
    EXPECT_TRUE(plan.empty());
    EXPECT_TRUE(plan.events().empty());
    plan.record(cmd(OpRender), {});
    draw(plan);
    plan.prepareRange(0, plan.size());
    size_t uses = 0;
    plan.forEachUse(0, plan.size(), [&](void *, uint8_t) {
        ++uses;
    });
    EXPECT_EQ(uses, 0u);
    EXPECT_EQ(plan.renderStates.size(), 1u);
}

TEST(SemanticPlan, EligibilityIgnoresRasterChangesButTracksBindingsAndOpaqueCalls)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    const auto initial = plan.eligibilityGeneration();
    Cmd scissor = cmd(OpScissor);
    scissor.a = 4;
    scissor.b = 8;
    plan.record(scissor, {});
    plan.record(cmd(OpBlendColor), {0, 0, 0, 0});
    EXPECT_EQ(plan.eligibilityGeneration(), initial);
    plan.record(binding(OpTexture, 2, 0, 7), {});
    const auto bound = plan.eligibilityGeneration();
    EXPECT_GT(bound, initial);
    plan.record(binding(OpTexture, 2, 0, 7), {});
    EXPECT_EQ(plan.eligibilityGeneration(), bound);
    plan.record(cmd(OpInvocation), {});
    EXPECT_GT(plan.eligibilityGeneration(), bound);
}

TEST(SemanticPlan, PreparedRangeCachesDistinctPipelinesAcrossEncoderSummaries)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpPipeline, 0, 0, 10), {});
    draw(plan);
    draw(plan);
    plan.record(binding(OpPipeline, 0, 0, 20), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    const size_t middle = plan.size();
    plan.prepareRange(0, middle);
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpPipeline, 0, 0, 10), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(middle, plan.size());
    plan.prepareRange(0, plan.size());
    const auto &pipelines = plan.renderPipelines(0, plan.size());
    ASSERT_EQ(pipelines.size(), 2u);
    EXPECT_EQ(pipelines[0], object(10));
    EXPECT_EQ(pipelines[1], object(20));
    const auto *cached = pipelines.data();
    for (unsigned wake = 0; wake < 100; ++wake) {
        plan.prepareRange(0, plan.size());
        EXPECT_EQ(plan.renderPipelines(0, plan.size()).data(), cached);
    }
}

TEST(SemanticPlan, TextureSamplingAndAttachmentLoadsRemainDistinct)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.addUses(0, {{object(1), raw::ResourceUse::Read | raw::ResourceUse::Write}});
    plan.record(binding(OpTexture, 2, 0, 2), {});
    draw(plan);
    plan.record(cmd(OpInvocation), {});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(1)], raw::ResourceUse::Read | raw::ResourceUse::Write);
    EXPECT_EQ(uses[object(2)], raw::ResourceUse::Read | raw::ResourceUse::Write | raw::ResourceUse::Sample);
}

TEST(SemanticPlan, ComputeAccessIsCachedPerNativeKernelAndPreservesEarlierDispatches)
{
    RMCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 10), {});
    plan.record(binding(OpKernelTexture, 3, 1, 20), {});
    plan.record(binding(OpKernelTexture, 3, 2, 30), {});
    const std::vector<uint64_t> dimensions{1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    unsigned builds = 0;
    auto readSource = [&](std::vector<raw::ResourceUse> &out) {
        ++builds;
        out = std::vector<raw::ResourceUse>{{object(10), raw::ResourceUse::Read},
                                            {object(20), raw::ResourceUse::Read | raw::ResourceUse::Sample}};
    };
    for (unsigned i = 0; i < 10000; ++i) {
        plan.record(cmd(OpDispatch), dimensions);
        plan.setKernelUses(plan.size() - 1, object(40), readSource);
    }
    EXPECT_EQ(builds, 1u);
    const size_t boundary = plan.size();
    plan.record(cmd(OpDispatch), dimensions);
    plan.setKernelUses(plan.size() - 1, object(41), [](std::vector<raw::ResourceUse> &out) {
        out = std::vector<raw::ResourceUse>{{object(20), raw::ResourceUse::Read | raw::ResourceUse::Write}};
    });
    std::map<void *, uint8_t> first, last;
    plan.forEachUse(1, boundary, [&](void *object, uint8_t access) {
        first[object] = access;
    });
    plan.forEachUse(boundary, plan.size(), [&](void *object, uint8_t access) {
        last[object] = access;
    });
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[object(10)], raw::ResourceUse::Read);
    EXPECT_EQ(first[object(20)], raw::ResourceUse::Read | raw::ResourceUse::Sample);
    EXPECT_EQ(first.count(object(30)), 0u);
    ASSERT_EQ(last.size(), 1u);
    EXPECT_EQ(last[object(20)], raw::ResourceUse::Read | raw::ResourceUse::Write);
}

TEST(SemanticPlan, DeferredComputeRebuildsRangeOwnershipFromTheOriginalSnapshot)
{
    RMCommandPlan plan;
    const std::vector<uint64_t> dimensions{1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 10, 64), {});
    Cmd pending = cmd(OpDispatch);
    pending.obj = object(100);
    plan.record(pending, dimensions);
    plan.record(binding(OpKernelBuffer, 3, 0, 20, 128), {});
    plan.record(pending, dimensions);
    plan.record(cmd(OpEnd), {});
    const auto boundary = plan.size();
    plan.prepareRange(0, boundary);
    plan.record(cmd(OpCompute), {});
    plan.record(pending, dimensions);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(boundary, plan.size());
    plan.prepareRange(0, plan.size());
    ASSERT_EQ(plan.pendingComputes(0, plan.size()), (std::vector<size_t>{1, 2, boundary + 1}));
    const auto &first = plan.kernelStates[plan.dispatch(1).state];
    const auto &second = plan.kernelStates[plan.dispatch(2).state];
    EXPECT_EQ(plan.buffers[2][first.buffers][0].offset, 64u);
    EXPECT_EQ(plan.buffers[2][second.buffers][0].offset, 128u);
    EXPECT_EQ(plan.dispatch(1).encoderBegin, 0u);
    EXPECT_EQ(plan.dispatch(boundary + 1).encoderBegin, boundary);
    plan.setKernelUses(1, object(200), [](std::vector<raw::ResourceUse> &out) {
        out = std::vector<raw::ResourceUse>{{object(10), raw::ResourceUse::Read}};
    });
    plan.setKernelUses(2, object(200), [](std::vector<raw::ResourceUse> &out) {
        out = std::vector<raw::ResourceUse>{{object(20), raw::ResourceUse::Write}};
    });
    plan.resolveDispatch(1, object(200), 4);
    plan.resolveDispatch(2, object(200), 0);
    plan.invalidateUses();
    plan.prepareRange(0, boundary);
    plan.prepareRange(0, plan.size());
    EXPECT_TRUE(plan.pendingComputes(0, boundary).empty());
    EXPECT_EQ(plan.pendingComputes(0, plan.size()), (std::vector<size_t>{boundary + 1}));
    EXPECT_EQ(plan.dispatch(1).kernel, object(200));
    EXPECT_EQ(plan.dispatch(1).surfaceSlots, 4u);
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, boundary, [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(10)], raw::ResourceUse::Read);
    EXPECT_EQ(uses[object(20)], raw::ResourceUse::Write);
    plan.clear();
    plan.record(cmd(OpCompute), {});
    plan.record(cmd(OpDispatch), dimensions);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    EXPECT_TRUE(plan.pendingComputes(0, plan.size()).empty());
}

TEST(SemanticPlan, ComputeAliasesUnionAccessAndExplicitWritesRemainDependencies)
{
    RMCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(cmd(OpDispatch), {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0});
    plan.setKernelUses(1, object(1), [](std::vector<raw::ResourceUse> &out) {
        out =
            std::vector<raw::ResourceUse>{{object(10), raw::ResourceUse::Read}, {object(10), raw::ResourceUse::Write}};
    });
    auto explicitUse = binding(OpKernelUse, 3, 0, 20, 2);
    plan.record(explicitUse, {});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(10)], raw::ResourceUse::Read | raw::ResourceUse::Write);
    EXPECT_EQ(uses[object(20)], raw::ResourceUse::Write | raw::ResourceUse::Explicit);
}

TEST(SemanticPlan, RepeatedIndirectDrawsReuseResourceSummaryAndChangesKeepEarlierOnes)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 10), {});
    Cmd indirect = cmd(OpDrawIndexedIndirect, 3);
    indirect.obj = object(20);
    indirect.obj2 = object(30);
    plan.record(indirect, {});
    const auto first = plan.operation(1).uses;
    for (unsigned draw = 0; draw < 10000; ++draw) {
        plan.record(indirect, {});
        EXPECT_EQ(plan.operation(plan.size() - 1).uses, first);
    }
    indirect.obj2 = object(40);
    plan.record(indirect, {});
    EXPECT_NE(plan.operation(plan.size() - 1).uses, first);
    std::map<void *, uint8_t> uses;
    plan.forEachUse(1, 2, [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses.size(), 3u);
    EXPECT_EQ(uses.count(object(30)), 1u);
    EXPECT_EQ(uses.count(object(40)), 0u);
}

TEST(SemanticPlan, ExplicitResidencySurvivesRangeUnionWithBoundResources)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 10), {});
    draw(plan);
    Cmd declared = binding(OpUse, 0, 0, 10);
    declared.a = 1;
    plan.record(declared, {});
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpCompute), {});
    declared = binding(OpKernelUse, 3, 0, 20);
    declared.c = 2;
    plan.record(declared, {});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(10)], raw::ResourceUse::Read | raw::ResourceUse::Explicit);
    EXPECT_EQ(uses[object(20)], raw::ResourceUse::Write | raw::ResourceUse::Explicit);
}

TEST(SemanticPlan, NativeVariantSnapshotsRemainDistinctAndRangeSummariesReuseThem)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd pipeline = binding(OpPipeline, 0, 0, 10);
    pipeline.obj2 = object(20);
    plan.record(pipeline, {});
    draw(plan);
    const size_t first = 1;
    for (unsigned i = 0; i < 100; ++i)
        draw(plan);
    pipeline.obj2 = object(30);
    plan.record(pipeline, {});
    draw(plan);
    const auto &old = plan.raster[plan.renderStates[plan.draw(first).state].raster];
    EXPECT_EQ(old.nativeVariant, object(20));
    plan.record(cmd(OpEnd), {});
    const size_t end = plan.size();
    plan.prepareRange(0, end);
    plan.record(cmd(OpRender), {});
    plan.record(pipeline, {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(end, plan.size());
    plan.prepareRange(0, plan.size());
    EXPECT_EQ(plan.renderVariants(0, plan.size()), (std::vector<void *>{object(20), object(30)}));
    EXPECT_EQ(plan.renderVariants(end, plan.size()), (std::vector<void *>{object(30)}));
    EXPECT_EQ(plan.renderPipelines(0, plan.size()), (std::vector<void *>{object(10)}));
    plan.clear();
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpPipeline, 0, 0, 10), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    EXPECT_TRUE(plan.renderVariants(0, plan.size()).empty());
}

}  // namespace

TEST(SemanticPlan, NonuniformRegionsKeepOneOriginalReplayOperationAndImmutableGrid)
{
    RMCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    Cmd dispatch = cmd(OpDispatch, 3, 1);
    dispatch.c = 8 | uint64_t(5) << 16 | uint64_t(3) << 32;
    dispatch.obj = object(40);
    plan.record(dispatch, {19, 11, 7, 3, 3, 3, 0, 0, 0, 0, 0, 0});
    ASSERT_EQ(plan.size(), 2u);
    ASSERT_EQ(plan.dispatch(1).regions.count, 8);
    EXPECT_EQ(plan.dispatch(1).grid, (std::array<uint64_t, 3>{19, 11, 7}));
    EXPECT_EQ(plan.dispatch(1).threads, (std::array<uint32_t, 3>{8, 5, 3}));
    plan.resolveDispatch(1, object(41), 0);
    plan.record(dispatch, {3, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(plan.dispatch(1).regions.count, 8);
    EXPECT_EQ(plan.dispatch(2).regions.count, 1);
    EXPECT_EQ(plan.dispatch(1).kernel, object(41));
    EXPECT_EQ(plan.dispatch(2).pending, object(40));
    EXPECT_EQ(plan.legacy(1).op, OpDispatch);
    EXPECT_TRUE(plan.dispatch(1).perThread);
}

namespace {

// Every sink call in order, as text: what the encoder is asked to do
struct CallLog {
    std::vector<std::string> calls;

    void buffer(uint32_t stage, uint32_t slot, const raw::BufferBinding &value)
    {
        calls.push_back("b" + std::to_string(stage) + "." + std::to_string(slot) + "=" +
                        std::to_string(reinterpret_cast<uintptr_t>(value.object)) + "+" + std::to_string(value.offset) +
                        "/" + std::to_string(int(value.source)));
    }

    void texture(uint32_t stage, uint32_t slot, void *value)
    {
        calls.push_back("t" + std::to_string(stage) + "." + std::to_string(slot) + "=" +
                        std::to_string(reinterpret_cast<uintptr_t>(value)));
    }

    void sampler(uint32_t stage, uint32_t slot, void *value)
    {
        calls.push_back("s" + std::to_string(stage) + "." + std::to_string(slot) + "=" +
                        std::to_string(reinterpret_cast<uintptr_t>(value)));
    }

    void raster(const raw::RasterState &, const raw::RasterState *)
    {
        calls.push_back("r");
    }

    void kernel(const raw::KernelState &, const raw::KernelState *)
    {
        calls.push_back("k");
    }
};

// The comparison of every slot of each changed table, as the encoder made it before tables recorded their edits
void applyEverySlot(const RMCommandPlan &plan, uint32_t index, uint32_t &applied, CallLog &log)
{
    if (applied == index)
        return;
    const auto &next = plan.renderStates[index];
    const auto *previous = applied == raw::kNoSnapshot ? nullptr : &plan.renderStates[applied];
    if (previous && previous->epoch != next.epoch)
        previous = nullptr;
    for (uint32_t stage = 0; stage < 2; ++stage) {
        if (!previous || previous->buffers[stage] != next.buffers[stage]) {
            const auto bindings = plan.buffers[stage][next.buffers[stage]];
            const auto old = plan.buffers[stage][previous ? previous->buffers[stage] : next.buffers[stage]];
            for (uint32_t slot = 0; slot < bindings.size(); ++slot)
                if (bindings[slot].source != raw::BufferBinding::Source::Unset &&
                    (!previous || bindings[slot] != old[slot]))
                    log.buffer(stage + 1, slot, bindings[slot]);
        }
        if (!previous || previous->textures[stage] != next.textures[stage]) {
            const auto bindings = plan.textures[stage][next.textures[stage]];
            const auto old = plan.textures[stage][previous ? previous->textures[stage] : next.textures[stage]];
            for (uint32_t slot = 0; slot < bindings.objects.size(); ++slot)
                if ((bindings.set & (1u << slot)) &&
                    (!previous || !(old.set & (1u << slot)) || bindings.objects[slot] != old.objects[slot]))
                    log.texture(stage + 1, slot, bindings.objects[slot]);
        }
        if (!previous || previous->samplers[stage] != next.samplers[stage]) {
            const auto bindings = plan.samplers[stage][next.samplers[stage]];
            const auto old = plan.samplers[stage][previous ? previous->samplers[stage] : next.samplers[stage]];
            for (uint32_t slot = 0; slot < bindings.objects.size(); ++slot)
                if ((bindings.set & (1u << slot)) &&
                    (!previous || !(old.set & (1u << slot)) || bindings.objects[slot] != old.objects[slot]))
                    log.sampler(stage + 1, slot, bindings.objects[slot]);
        }
    }
    if (!previous || previous->raster != next.raster)
        log.raster(plan.raster[next.raster], previous ? &plan.raster[previous->raster] : nullptr);
    applied = index;
}

Cmd bytes(uint8_t stage, uint16_t slot, uint32_t chunk, uint32_t offset)
{
    Cmd c = cmd(OpBytes, stage, slot);
    c.a = offset;
    c.b = 64;
    c.c = chunk;
    return c;
}

// Core Animation's encoder: per draw new vertex and fragment bytes, a texture and sampler now and then, a pipeline
// switch, one encoder ending and the next starting with the same bindings
RMCommandPlan caPlan()
{
    RMCommandPlan plan;
    for (uint32_t pass = 0; pass < 3; ++pass) {
        plan.record(cmd(OpRender), {});
        for (uint32_t i = 0; i < 40; ++i) {
            for (uint16_t slot : {1, 2, 3})
                plan.record(bytes(1, slot, i % 2, i * 256 + slot * 64), {});
            for (uint16_t slot : {1, 2, 5, 6})
                plan.record(bytes(2, slot, i % 2, i * 256 + slot * 64 + 32), {});
            if (i % 7 == 0)
                plan.record(binding(OpTexture, 2, uint16_t(3 + i % 2), 100 + i), {});
            if (i % 11 == 0)
                plan.record(binding(OpSampler, 2, 1, 200 + i % 3), {});
            if (i % 5 == 0)
                plan.record(binding(OpBuffer, 1, 9, 300 + i, 16 * i), {});
            if (i % 13 == 4)
                plan.record(binding(OpBufferOffset, 1, 9, 0, 64 * i), {});
            if (i % 4 == 0)
                plan.record(binding(OpPipeline, 0, 0, 400 + i % 3), {});
            draw(plan, i);
        }
        plan.record(cmd(OpEnd), {});
    }
    return plan;
}

}  // namespace

TEST(SemanticPlan, AppliedStateIsTheEverySlotComparisonsWithOnlyTheEditedSlotsCompared)
{
    const RMCommandPlan plan = caPlan();
    CallLog masked, every;
    raw::StateCursor cursor;
    uint32_t applied = raw::kNoSnapshot;
    for (size_t at = 0; at < plan.size(); ++at) {
        const Op op = plan.operation(at).op;
        if (isPlanDraw(op)) {
            raw::applyRenderState(plan, plan.draw(at).state, cursor, masked);
            applyEverySlot(plan, plan.draw(at).state, applied, every);
        } else if (op == OpRender || op == OpEnd) {
            cursor.reset();
            applied = raw::kNoSnapshot;
        }
    }
    EXPECT_EQ(masked.calls, every.calls);
    EXPECT_GT(masked.calls.size(), 0u);
    printf("binding slots compared per draw: %.1f of %.1f\n", double(cursor.slotsCompared) / 120,
           double(cursor.slotsInTables) / 120);
    EXPECT_LT(cursor.slotsCompared * 4, cursor.slotsInTables);
}

TEST(SemanticPlan, ASnapshotNotNextAfterTheAppliedOneComparesEverySlot)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(bytes(1, 1, 0, 0), {});
    draw(plan, 0);
    plan.record(bytes(1, 2, 0, 64), {});
    draw(plan, 1);
    plan.record(bytes(1, 3, 0, 128), {});
    draw(plan, 2);
    const uint32_t first = plan.renderStates[plan.draw(1).state].buffers[0],
                   third = plan.renderStates[plan.draw(3).state].buffers[0];
    EXPECT_EQ(plan.buffers[0].changedFrom(first, first + 1), 1u << 2);
    EXPECT_EQ(plan.buffers[0].changedFrom(first, third), ~0u);
    CallLog masked, every;
    raw::StateCursor cursor;
    uint32_t applied = raw::kNoSnapshot;
    for (size_t at : {size_t(1), size_t(3)}) {
        raw::applyRenderState(plan, plan.draw(at).state, cursor, masked);
        applyEverySlot(plan, plan.draw(at).state, applied, every);
    }
    EXPECT_EQ(masked.calls, every.calls);
}
