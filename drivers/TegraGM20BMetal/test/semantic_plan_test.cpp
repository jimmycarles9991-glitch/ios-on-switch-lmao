// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../TegraGM20BMetalPlanApply.h"
#include "semantic_plan_fixture.h"
#include <vector>

namespace {

// A command buffer's own plan, which the recording pool's replaces, allocates nothing; one recorded into as made (the
// pool had none) reads its operations of no resource as using none
TEST(SemanticPlan, AConstructedPlanHoldsNoStorageAndItsFirstOperationsUseNothing)
{
    SwitchMetalCommandPlan plan;
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
    SwitchMetalCommandPlan plan;
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
    SwitchMetalCommandPlan plan;
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
    SwitchMetalCommandPlan plan;
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

// Boot 21: Core Animation's and RenderBox's vertex bytes before an encoder's first draw keep a shadow copy, whose index
// was read as an attributeStride, so every range the driver refused with one failed as "a vertex buffer bound with an
// attribute stride"; a real stride form's bytes named a shadow at its stride
TEST(SemanticPlan, ABytesShadowIsNeverItsAttributeStride)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd shadowed = bytes(1, 0, 0);
    shadowed.shadow = 1;
    plan.record(shadowed, {});
    Cmd strided = bytes(1, 1, 256);
    strided.x = 17;
    plan.record(strided, {});
    draw(plan);
    const auto &state = plan.renderStates[plan.draw(1).state];
    const gm20b::BufferBinding &first = plan.buffers[0][state.buffers[0]][0];
    const gm20b::BufferBinding &second = plan.buffers[0][state.buffers[0]][1];
    EXPECT_EQ(first.stride, gm20b::kStaticStride);
    EXPECT_EQ(first.shadow, 1u);
    EXPECT_EQ(second.stride, 16u);
    EXPECT_EQ(second.shadow, 0u);
}

TEST(SemanticPlan, ADrawsSetBytesCaptureCopiesOnlyTheChunkOfSlotsTheyEdited)
{
    SwitchMetalCommandPlan plan;
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
            EXPECT_EQ(bindings[slot].source, gm20b::BufferBinding::Source::Bytes);
            EXPECT_EQ(bindings[slot].offset, uint64_t(d) * 1024 + slot * 256);
        }
        EXPECT_EQ(bindings[20].object, object(7));
        EXPECT_EQ(bindings[20].offset, 4096u);
        EXPECT_EQ(bindings[5].source, gm20b::BufferBinding::Source::Unset);
        uint32_t set = 0;
        for (const gm20b::BufferBinding &b : bindings)
            set += b.source != gm20b::BufferBinding::Source::Unset;
        EXPECT_EQ(set, 5u);
    }
    // the first capture's eight chunks, then one a draw: far below a whole table a draw
    EXPECT_LT(plan.buffers[0].storageBytes(), kDraws * sizeof(gm20b::BufferBindings) / 3);
}

TEST(SemanticPlan, ADrawsTextureCaptureCopiesOnlyTheChunkOfSlotsItBound)
{
    SwitchMetalCommandPlan plan;
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
        gm20b::TextureSnapshots::Mask set{};
        for (uint32_t slot : {0, 1, 9})
            gm20b::addSlot(set, slot);
        EXPECT_TRUE(textures.set == set);
    }
    // a capture's chunk of slots and its chunk list, half of a whole table's copy and its mask at most
    EXPECT_LT(plan.textures[1].storageBytes(),
              kDraws * gm20b::kMaskSlots<gm20b::TextureSnapshots::Mask> * sizeof(void *) / 2);
}

TEST(SemanticPlan, AWordKeyedMapKeepsItsStorageAcrossClears)
{
    gm20b::StampMap<std::tuple<uint32_t, uintptr_t, uintptr_t>, uint32_t, gm20b::WordsHash> map;
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
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(bytes(1, 3, 256), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    plan.record(bytes(1, 30, 512), {});
    draw(plan);
    const auto &state = plan.renderStates[plan.draw(plan.size() - 1).state];
    const auto bindings = plan.buffers[0][state.buffers[0]];
    EXPECT_EQ(bindings[3].source, gm20b::BufferBinding::Source::Unset);
    EXPECT_EQ(bindings[30].offset, 512u);
    EXPECT_EQ(bindings[30].source, gm20b::BufferBinding::Source::Bytes);
}

TEST(SemanticPlan, APointerMapKeepsItsStorageAcrossClears)
{
    gm20b::PointerMap<uint32_t> map;
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

TEST(SemanticPlan, PageSpacedPointerKeysKeepLookupProbesBounded)
{
    struct Key {
        const void *pointer = nullptr;
        uint32_t *comparisons = nullptr;

        bool operator==(const Key &other) const
        {
            ++*comparisons;
            return pointer == other.pointer;
        }
    };

    struct Hash {
        size_t operator()(const Key &key) const
        {
            return gm20b::PointerHash{}(key.pointer);
        }
    };

    constexpr uint32_t count = 96;
    for (uintptr_t stride : {16u, 64u, 256u, 4096u, 16384u})
        for (uintptr_t base : {uintptr_t(0), uintptr_t(0x110), uintptr_t(0xffff000000000000ull)}) {
            uint32_t comparisons = 0;
            gm20b::StampMap<Key, uint32_t, Hash> map;
            const auto key = [&](uint32_t i) {
                return Key{object(base + i * stride), &comparisons};
            };
            for (uint32_t i = 0; i < count; ++i)
                EXPECT_TRUE(map.insert(key(i), i).second);
            comparisons = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t *value = map.find(key(i));
                ASSERT_NE(value, nullptr);
                EXPECT_EQ(*value, i);
            }
            EXPECT_LE(comparisons, 2 * count) << "stride " << stride << ", base " << base;
        }
}

}  // namespace
