// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../TegraGM20BMetalPlanApply.h"
#include "semantic_plan_fixture.h"
#include <map>
#include <vector>

namespace {

TEST(SemanticPlan, CachedRangeSummariesUnionAccessAndExcludeOtherRanges)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.addUses(0, {{object(1), gm20b::ResourceUse::Write}});
    plan.record(binding(OpBuffer, 1, 0, 2), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 2), {});
    Cmd dispatch = cmd(OpDispatch);
    dispatch.c = 1 | uint64_t(1) << 16 | uint64_t(1) << 32;
    plan.record(dispatch, words({1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0}));
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, 3);
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, 3, [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses.size(), 2u);
    EXPECT_EQ(uses[object(2)], gm20b::ResourceUse::Read);
    uses.clear();
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(2)], gm20b::ResourceUse::Read | gm20b::ResourceUse::Write);
    std::vector<void *> writes;
    plan.forEachWritten(0, 3, [&](void *object) {
        writes.push_back(object);
    });
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0], object(1));
}

TEST(SemanticPlan, ReflectedUsesReplaceUnusedBindingResources)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpTexture, 2, 0, 1), {});
    plan.record(binding(OpTexture, 2, 1, 2), {});
    draw(plan);
    plan.setRenderUses(plan.draw(1).state, {{object(1), gm20b::ResourceUse::Read}});
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
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd indexed = binding(OpDrawIndexed, 3, 0, 21, 256);
    indexed.a = 6;
    indexed.b = 1;
    plan.record(indexed, words({4, uint64_t(int64_t(-7)), 9}));
    const auto &draw = plan.draw(1);
    EXPECT_EQ(draw.baseVertex, -7);
    EXPECT_EQ(draw.baseInstance, 9u);
    EXPECT_EQ(draw.instances, 4u);
    EXPECT_EQ(draw.indexOffset, 256u);
}

TEST(SemanticPlan, IndirectArgumentsAreReferencesNeverCpuReads)
{
    SwitchMetalCommandPlan plan;
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
    EXPECT_EQ(uses[object(1)], gm20b::ResourceUse::Read);
    EXPECT_EQ(uses[object(3)], gm20b::ResourceUse::Read);
}

TEST(SemanticPlan, RecycledStorageDropsAllSemanticState)
{
    SwitchMetalCommandPlan plan;
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

TEST(SemanticPlan, EligibilityIgnoresRasterChangesButTracksBindings)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    const auto initial = plan.eligibilityGeneration();
    Cmd scissor = cmd(OpScissor);
    scissor.a = 4;
    scissor.b = 8;
    plan.record(scissor, {});
    plan.record(cmd(OpBlendColor), words({0, 0, 0, 0}));
    EXPECT_EQ(plan.eligibilityGeneration(), initial);
    plan.record(binding(OpTexture, 2, 0, 7), {});
    const auto bound = plan.eligibilityGeneration();
    EXPECT_GT(bound, initial);
    plan.record(binding(OpTexture, 2, 0, 7), {});
    EXPECT_EQ(plan.eligibilityGeneration(), bound);
}

TEST(SemanticPlan, PreparedRangeCachesDistinctPipelinesAcrossEncoderSummaries)
{
    SwitchMetalCommandPlan plan;
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
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.addUses(0, {{object(1), gm20b::ResourceUse::Read | gm20b::ResourceUse::Write}});
    plan.record(binding(OpTexture, 2, 0, 2), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(1)], gm20b::ResourceUse::Read | gm20b::ResourceUse::Write);
    EXPECT_EQ(uses[object(2)], gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample);
}

TEST(SemanticPlan, ComputeAccessIsCachedPerNativeKernelAndPreservesEarlierDispatches)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 10), {});
    plan.record(binding(OpKernelTexture, 3, 1, 20), {});
    plan.record(binding(OpKernelTexture, 3, 2, 30), {});
    const std::vector<uint64_t> dimensions{1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    unsigned builds = 0;
    auto readSource = [&](std::vector<gm20b::ResourceUse> &out) {
        ++builds;
        out = std::vector<gm20b::ResourceUse>{{object(10), gm20b::ResourceUse::Read},
                                              {object(20), gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample}};
    };
    for (unsigned i = 0; i < 10000; ++i) {
        plan.record(cmd(OpDispatch), dimensions.data());
        plan.setKernelUses(plan.size() - 1, object(40), readSource);
    }
    EXPECT_EQ(builds, 1u);
    const size_t boundary = plan.size();
    plan.record(cmd(OpDispatch), dimensions.data());
    plan.setKernelUses(plan.size() - 1, object(41), [](std::vector<gm20b::ResourceUse> &out) {
        out = std::vector<gm20b::ResourceUse>{{object(20), gm20b::ResourceUse::Read | gm20b::ResourceUse::Write}};
    });
    std::map<void *, uint8_t> first, last;
    plan.forEachUse(1, boundary, [&](void *object, uint8_t access) {
        first[object] = access;
    });
    plan.forEachUse(boundary, plan.size(), [&](void *object, uint8_t access) {
        last[object] = access;
    });
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[object(10)], gm20b::ResourceUse::Read);
    EXPECT_EQ(first[object(20)], gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample);
    EXPECT_EQ(first.count(object(30)), 0u);
    ASSERT_EQ(last.size(), 1u);
    EXPECT_EQ(last[object(20)], gm20b::ResourceUse::Read | gm20b::ResourceUse::Write);
}

TEST(SemanticPlan, DeferredComputeRebuildsRangeOwnershipFromTheOriginalSnapshot)
{
    SwitchMetalCommandPlan plan;
    const std::vector<uint64_t> dimensions{1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 10, 64), {});
    Cmd pending = cmd(OpDispatch);
    pending.obj = object(100);
    plan.record(pending, dimensions.data());
    plan.record(binding(OpKernelBuffer, 3, 0, 20, 128), {});
    plan.record(pending, dimensions.data());
    plan.record(cmd(OpEnd), {});
    const auto boundary = plan.size();
    plan.prepareRange(0, boundary);
    plan.record(cmd(OpCompute), {});
    plan.record(pending, dimensions.data());
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
    plan.setKernelUses(1, object(200), [](std::vector<gm20b::ResourceUse> &out) {
        out = std::vector<gm20b::ResourceUse>{{object(10), gm20b::ResourceUse::Read}};
    });
    plan.setKernelUses(2, object(200), [](std::vector<gm20b::ResourceUse> &out) {
        out = std::vector<gm20b::ResourceUse>{{object(20), gm20b::ResourceUse::Write}};
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
    EXPECT_EQ(plan.legacy(1).a, 4u);
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, boundary, [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(10)], gm20b::ResourceUse::Read);
    EXPECT_EQ(uses[object(20)], gm20b::ResourceUse::Write);
    plan.clear();
    plan.record(cmd(OpCompute), {});
    plan.record(cmd(OpDispatch), dimensions.data());
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    EXPECT_TRUE(plan.pendingComputes(0, plan.size()).empty());
}

TEST(SemanticPlan, ComputeAliasesUnionAccessAndExplicitWritesRemainDependencies)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(cmd(OpDispatch), words({1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0}));
    plan.setKernelUses(1, object(1), [](std::vector<gm20b::ResourceUse> &out) {
        out = std::vector<gm20b::ResourceUse>{{object(10), gm20b::ResourceUse::Read},
                                              {object(10), gm20b::ResourceUse::Write}};
    });
    auto explicitUse = binding(OpKernelUse, 3, 0, 20, 2);
    plan.record(explicitUse, {});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(10)], gm20b::ResourceUse::Read | gm20b::ResourceUse::Write);
    EXPECT_EQ(uses[object(20)], gm20b::ResourceUse::Write | gm20b::ResourceUse::Explicit);
}

TEST(SemanticPlan, RepeatedIndirectDrawsReuseResourceSummaryAndChangesKeepEarlierOnes)
{
    SwitchMetalCommandPlan plan;
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
    SwitchMetalCommandPlan plan;
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
    EXPECT_EQ(uses[object(10)], gm20b::ResourceUse::Read | gm20b::ResourceUse::Explicit);
    EXPECT_EQ(uses[object(20)], gm20b::ResourceUse::Write | gm20b::ResourceUse::Explicit);
}

TEST(SemanticPlan, NativeVariantSnapshotsRemainDistinctAndRangeSummariesReuseThem)
{
    SwitchMetalCommandPlan plan;
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
