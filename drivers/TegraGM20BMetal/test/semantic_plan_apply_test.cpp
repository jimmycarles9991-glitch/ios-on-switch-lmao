// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../TegraGM20BMetalPlanApply.h"
#include "semantic_plan_fixture.h"
#include <cstdio>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

TEST(SemanticPlan, NonuniformRegionsKeepOneOriginalReplayOperationAndImmutableGrid)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    Cmd dispatch = cmd(OpDispatch, 3, 1);
    dispatch.c = 8 | uint64_t(5) << 16 | uint64_t(3) << 32;
    dispatch.obj = object(40);
    plan.record(dispatch, words({19, 11, 7, 3, 3, 3, 0, 0, 0, 0, 0, 0}));
    ASSERT_EQ(plan.size(), 2u);
    ASSERT_EQ(plan.dispatch(1).regions.count, 8);
    EXPECT_EQ(plan.dispatch(1).grid, (std::array<uint64_t, 3>{19, 11, 7}));
    EXPECT_EQ(plan.dispatch(1).threads, (std::array<uint32_t, 3>{8, 5, 3}));
    plan.resolveDispatch(1, object(41), 0);
    plan.record(dispatch, words({3, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0}));
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

    void buffer(uint32_t stage, uint32_t slot, const gm20b::BufferBinding &value)
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

    void raster(const gm20b::RasterState &, const gm20b::RasterState *)
    {
        calls.push_back("r");
    }

    void kernel(const gm20b::KernelState &, const gm20b::KernelState *)
    {
        calls.push_back("k");
    }
};

// The comparison of every slot of each changed table, as the encoder made it before tables recorded their edits
void applyEverySlot(const SwitchMetalCommandPlan &plan, uint32_t index, uint32_t &applied, CallLog &log)
{
    if (applied == index)
        return;
    const auto &next = plan.renderStates[index];
    const auto *previous = applied == gm20b::kNoSnapshot ? nullptr : &plan.renderStates[applied];
    if (previous && previous->epoch != next.epoch)
        previous = nullptr;
    for (uint32_t stage = 0; stage < 2; ++stage) {
        if (!previous || previous->buffers[stage] != next.buffers[stage]) {
            const auto bindings = plan.buffers[stage][next.buffers[stage]];
            const auto old = plan.buffers[stage][previous ? previous->buffers[stage] : next.buffers[stage]];
            for (uint32_t slot = 0; slot < bindings.size(); ++slot)
                if (bindings[slot].source != gm20b::BufferBinding::Source::Unset &&
                    (!previous || bindings[slot] != old[slot]))
                    log.buffer(stage + 1, slot, bindings[slot]);
        }
        if (!previous || previous->textures[stage] != next.textures[stage]) {
            const auto bindings = plan.textures[stage][next.textures[stage]];
            const auto old = plan.textures[stage][previous ? previous->textures[stage] : next.textures[stage]];
            for (uint32_t slot = 0; slot < bindings.objects.size(); ++slot)
                if (gm20b::hasSlot(bindings.set, slot) &&
                    (!previous || !gm20b::hasSlot(old.set, slot) || bindings.objects[slot] != old.objects[slot]))
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
SwitchMetalCommandPlan caPlan()
{
    SwitchMetalCommandPlan plan;
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
    const SwitchMetalCommandPlan plan = caPlan();
    CallLog masked, every;
    gm20b::StateCursor cursor;
    uint32_t applied = gm20b::kNoSnapshot;
    for (size_t at = 0; at < plan.size(); ++at) {
        const Op op = plan.operation(at).op;
        if (isPlanDraw(op)) {
            gm20b::applyRenderState(plan, plan.draw(at).state, cursor, masked);
            applyEverySlot(plan, plan.draw(at).state, applied, every);
        } else if (op == OpRender || op == OpEnd) {
            cursor.reset();
            applied = gm20b::kNoSnapshot;
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
    SwitchMetalCommandPlan plan;
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
    gm20b::StateCursor cursor;
    uint32_t applied = gm20b::kNoSnapshot;
    for (size_t at : {size_t(1), size_t(3)}) {
        gm20b::applyRenderState(plan, plan.draw(at).state, cursor, masked);
        applyEverySlot(plan, plan.draw(at).state, applied, every);
    }
    EXPECT_EQ(masked.calls, every.calls);
}

// A render stage's texture slots are the identity's (stage_textures.h): 24B119's 128, whose binds at slots 31, 64 and
// 127 are captured, compared and applied as a bind at slot 0 is, an edit past the first mask word compared alone; on
// 21H461 a bind past its 31 is not kept. A kernel's table keeps 31 slots on both.
TEST(SemanticPlan, ARenderStagesTextureSlotsAreTheIdentitysAndAKernelsAre31)
{
    static_assert(gm20b::kStageTextures > 32 || std::is_same_v<gm20b::TextureSnapshots::Mask, uint32_t>,
                  "21H461's tables keep the one-word masks they had");
    static_assert(std::is_same_v<gm20b::KernelTextureSnapshots::Mask, uint32_t>);
    const std::vector<uint16_t> slots =
        gm20b::kStageTextures == 128 ? std::vector<uint16_t>{31, 64, 127} : std::vector<uint16_t>{29, 30};
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    for (uint8_t stage : {1, 2})
        for (uint16_t slot : slots)
            plan.record(binding(OpTexture, stage, slot, 1000 * stage + slot), {});
    plan.record(binding(OpTexture, 2, uint16_t(gm20b::kStageTextures), 7), {});
    draw(plan, 0);
    plan.record(binding(OpTexture, 2, slots.back(), 9), {});
    draw(plan, 1);
    const auto &first = plan.renderStates[plan.draw(plan.size() - 2).state];
    const auto &second = plan.renderStates[plan.draw(plan.size() - 1).state];
    for (uint32_t stage = 0; stage < 2; ++stage) {
        const auto textures = plan.textures[stage][first.textures[stage]];
        EXPECT_EQ(gm20b::slotCount(textures.set), slots.size()) << "stage " << stage + 1;
        for (uint16_t slot : slots) {
            EXPECT_TRUE(gm20b::hasSlot(textures.set, slot)) << "stage " << stage + 1 << " slot " << slot;
            EXPECT_EQ(textures.objects[slot], object(1000 * (stage + 1) + slot)) << "slot " << slot;
        }
    }
    EXPECT_EQ(plan.textures[1][second.textures[1]].objects[slots.back()], object(9));
    EXPECT_TRUE(plan.textures[1].changedFrom(first.textures[1], second.textures[1]) ==
                gm20b::oneSlot<gm20b::TextureSnapshots::Mask>(slots.back()));
    CallLog masked, every;
    gm20b::StateCursor cursor;
    uint32_t applied = gm20b::kNoSnapshot;
    for (size_t at : {plan.size() - 2, plan.size() - 1}) {
        gm20b::applyRenderState(plan, plan.draw(at).state, cursor, masked);
        applyEverySlot(plan, plan.draw(at).state, applied, every);
    }
    EXPECT_EQ(masked.calls, every.calls);
    const std::string rebound = "t2." + std::to_string(slots.back()) + "=9";
    EXPECT_EQ(masked.calls.back(), rebound);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelTexture, 3, 30, 11), {});
    plan.record(binding(OpKernelTexture, 3, 31, 12), {});
    EXPECT_EQ(plan.kernelTextures.current().set, 1u << 30);
}

// A draw whose caller resolves its state's uses after it (resolvesUses) leaves them to the caller only while the state
// is not final: an indexed draw of a state resolved already names that state's uses and its index buffer
TEST(SemanticPlan, AnIndexedDrawOfAFinalStateNamesItsIndexBufferThoughItsCallerResolves)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpTexture, 2, 0, 43), {});
    Cmd indexed = binding(OpDrawIndexed, 3, 0, 21);
    indexed.a = 6;
    indexed.b = 1;
    ASSERT_TRUE(plan.record(indexed, words({1, 0, 0}), 3, true));
    const uint32_t state = plan.draw(plan.size() - 1).state;
    uint32_t builds = 0;
    plan.resolveRenderUses(state, object(90), [&](std::vector<gm20b::ResourceUse> &out) {
        ++builds;
        out = {{object(43), uint8_t(gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample)}};
    });
    ASSERT_TRUE(plan.renderStates[state].usesFinal);
    ASSERT_TRUE(plan.record(indexed, words({1, 0, 0}), 3, true));
    const size_t second = plan.size() - 1;
    EXPECT_EQ(plan.draw(second).state, state);
    EXPECT_EQ(builds, 1u);
    std::map<void *, uint8_t> uses;
    plan.forEachUse(second, second + 1, [&](void *resource, uint8_t access) {
        uses[resource] = access;
    });
    EXPECT_EQ(uses,
              (std::map<void *, uint8_t>{{object(43), uint8_t(gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample)},
                                         {object(21), gm20b::ResourceUse::Read}}));
}
