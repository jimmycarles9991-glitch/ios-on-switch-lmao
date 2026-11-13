// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/RawMetalPlanExecution.h"

namespace {

void *object(uintptr_t value)
{
    return reinterpret_cast<void *>(value);
}

constexpr uint8_t read = raw::ResourceUse::Read;
constexpr uint8_t write = raw::ResourceUse::Write;
constexpr uint8_t sample = raw::ResourceUse::Sample;

void resource(RMCommandPlan &plan, uintptr_t value, RMResourceKind kind, uint8_t roles = 0, uintptr_t buffer = 0)
{
    plan.registerResource(object(value), {object(buffer), kind, roles});
}

size_t pass(RMCommandPlan &plan, const std::vector<raw::ResourceUse> &uses)
{
    const auto at = plan.size();
    plan.record(cmd(OpRender), {});
    plan.addUses(at, uses);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(at, plan.size());
    return at;
}

std::vector<RMRangeResource> resources(const RMCommandPlan &plan, size_t from, size_t to, RMResourceKind kind)
{
    std::vector<RMRangeResource> result;
    plan.forEachResource(from, to, kind, [&](const auto &use) {
        result.push_back(use);
    });
    return result;
}

TEST(PlanResources, OneMergedResourceCarriesItsKindAliasAndExactAccess)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Texture, RMResourceFacts::Converted, 2);
    resource(plan, 2, RMResourceKind::Buffer, 0, 2);
    resource(plan, 3, RMResourceKind::Sampler);
    pass(plan,
         {{object(1), read | sample}, {object(2), read}, {object(1), write}, {object(3), read}, {object(4), read}});
    const auto textures = resources(plan, 0, plan.size(), RMResourceKind::Texture);
    ASSERT_EQ(textures.size(), 1u);
    EXPECT_EQ(textures[0].object, object(1));
    EXPECT_EQ(textures[0].facts.cpuBuffer, object(2));
    EXPECT_EQ(textures[0].facts.roles, RMResourceFacts::Converted);
    EXPECT_EQ(textures[0].access, read | write | sample);
    EXPECT_EQ(resources(plan, 0, plan.size(), RMResourceKind::Buffer).size(), 1u);
    EXPECT_EQ(resources(plan, 0, plan.size(), RMResourceKind::Sampler).size(), 1u);
    const auto others = resources(plan, 0, plan.size(), RMResourceKind::Other);
    ASSERT_EQ(others.size(), 1u);
    EXPECT_EQ(others[0].object, object(4));
}

TEST(PlanResources, NewRetainedObjectsPreserveCompletedEncoderSummaries)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Buffer, 0, 1);
    pass(plan, {{object(1), read}});
    const RMRangeResource *first = nullptr;
    plan.forEachResource(0, 2, RMResourceKind::Buffer, [&](const auto &use) {
        first = &use;
    });
    resource(plan, 2, RMResourceKind::Texture);
    pass(plan, {{object(2), write}});
    plan.prepareRange(0, 2);
    plan.forEachResource(0, 2, RMResourceKind::Buffer, [&](const auto &use) {
        EXPECT_EQ(&use, first);
    });
    plan.prepareRange(0, plan.size());
    EXPECT_EQ(resources(plan, 0, plan.size(), RMResourceKind::Buffer).size(), 1u);
    EXPECT_EQ(resources(plan, 0, plan.size(), RMResourceKind::Texture).size(), 1u);
}

TEST(PlanResources, RegistrationIsImmutableUntilTheRecordingIsRecycled)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Texture, RMResourceFacts::Converted, 2);
    resource(plan, 1, RMResourceKind::Buffer, 0, 3);
    pass(plan, {{object(1), read}});
    auto use = resources(plan, 0, plan.size(), RMResourceKind::Texture);
    ASSERT_EQ(use.size(), 1u);
    EXPECT_EQ(use[0].facts.cpuBuffer, object(2));
    plan.clear();
    resource(plan, 1, RMResourceKind::Buffer, 0, 3);
    pass(plan, {{object(1), read}});
    use = resources(plan, 0, plan.size(), RMResourceKind::Buffer);
    ASSERT_EQ(use.size(), 1u);
    EXPECT_EQ(use[0].facts.cpuBuffer, object(3));
    EXPECT_TRUE(resources(plan, 0, plan.size(), RMResourceKind::Texture).empty());
}

TEST(PlanResources, PlanarFeedbackRefusesAtEncoderCloseWithoutChangingReplay)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Texture, RMResourceFacts::Converted | RMResourceFacts::PlanarTarget);
    pass(plan, {{object(1), write}, {object(1), read | sample}});
    EXPECT_EQ(plan.nativeRangeRefusal(0, plan.size()), object(1));
    plan.setRefused(0, false);
    EXPECT_TRUE(plan.begin(0).refused);
    raw::ExecutionPlan execution;
    EXPECT_EQ(makeExecutionPlan(plan, false, execution,
                                [](const auto &) {
                                    return true;
                                }),
              RMPlanError::None);
    ASSERT_EQ(execution.ranges().size(), 1u);
    EXPECT_EQ(execution.ranges()[0].kind, raw::ExecutionKind::compatibility);
    EXPECT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan.legacy(0).op, OpRender);
    EXPECT_EQ(plan.legacy(1).op, OpEnd);
}

TEST(PlanResources, FeedbackInTheJoinedRangeDoesNotPoisonTheIndividualPasses)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Texture, RMResourceFacts::Converted | RMResourceFacts::PlanarTarget);
    pass(plan, {{object(1), write}});
    pass(plan, {{object(1), read | sample}});
    EXPECT_EQ(plan.nativeRangeRefusal(0, 2), nullptr);
    EXPECT_EQ(plan.nativeRangeRefusal(2, 4), nullptr);
    plan.prepareRange(0, 4);
    EXPECT_EQ(plan.nativeRangeRefusal(0, 4), object(1));
    EXPECT_FALSE(plan.begin(0).refused);
    EXPECT_FALSE(plan.begin(2).refused);
}

TEST(PlanResources, EventsSeparateConversionEpochs)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Texture, RMResourceFacts::Converted | RMResourceFacts::PlanarTarget);
    pass(plan, {{object(1), write}});
    Cmd event = cmd(OpSignalEvent);
    event.obj = object(2);
    event.c = UINT64_C(0x100000001);
    plan.record(event, {});
    pass(plan, {{object(1), read | sample}});
    raw::ExecutionPlan execution;
    ASSERT_EQ(makeExecutionPlan(plan, false, execution,
                                [](const auto &) {
                                    return true;
                                }),
              RMPlanError::None);
    ASSERT_EQ(execution.ranges().size(), 3u);
    EXPECT_EQ(execution.ranges()[1].kind, raw::ExecutionKind::signal);
    EXPECT_EQ(plan.event(2).value, event.c);
    for (const auto &range : execution.ranges()) {
        plan.prepareRange(range.from, range.to);
        EXPECT_EQ(plan.nativeRangeRefusal(range.from, range.to), nullptr);
    }
}

TEST(PlanResources, OrdinaryFeedbackAndReadOnlyPlanarSamplingRemainEligible)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Texture);
    resource(plan, 2, RMResourceKind::Texture, RMResourceFacts::Converted);
    resource(plan, 3, RMResourceKind::Texture, RMResourceFacts::Converted | RMResourceFacts::PlanarTarget);
    pass(plan, {{object(1), write | sample}, {object(2), write | sample}, {object(3), read | sample}});
    EXPECT_EQ(plan.nativeRangeRefusal(0, plan.size()), nullptr);
}

TEST(PlanResources, PendingComputeDoesNotRefuseItsConservativeWritableSummary)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Texture, RMResourceFacts::Converted | RMResourceFacts::PlanarTarget);
    plan.record(cmd(OpCompute), {});
    Cmd bind = cmd(OpKernelTexture, 3, 0);
    bind.obj = object(1);
    plan.record(bind, {});
    Cmd dispatch = cmd(OpDispatch);
    dispatch.obj = object(2);
    dispatch.c = UINT64_C(1) | UINT64_C(1) << 16 | UINT64_C(1) << 32;
    plan.record(dispatch, {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, 3);
    EXPECT_EQ(plan.nativeRangeRefusal(0, 3), nullptr);
    plan.setRefused(0, false);
    EXPECT_FALSE(plan.begin(0).refused);
    plan.setKernelUses(1, object(3), [](std::vector<raw::ResourceUse> &out) {
        out = std::vector<raw::ResourceUse>{{object(1), read | sample}};
    });
    plan.resolveDispatch(1, object(3), 0);
    plan.invalidateUses();
    plan.prepareRange(0, 3);
    EXPECT_EQ(plan.nativeRangeRefusal(0, 3), nullptr);
    const auto use = resources(plan, 0, 3, RMResourceKind::Texture);
    ASSERT_EQ(use.size(), 1u);
    EXPECT_EQ(use[0].access, read | sample);
    plan.setKernelUses(1, object(4), [](std::vector<raw::ResourceUse> &out) {
        out = std::vector<raw::ResourceUse>{{object(1), read | write | sample}};
    });
    plan.invalidateUses();
    plan.prepareRange(0, 3);
    EXPECT_EQ(plan.nativeRangeRefusal(0, 3), object(1));
}

TEST(PlanResources, RebuildingACombinedRangeAlsoRestoresPerEncoderTypedResources)
{
    RMCommandPlan plan;
    resource(plan, 1, RMResourceKind::Texture);
    pass(plan, {{object(1), read | sample}});
    pass(plan, {{object(1), write}});
    plan.invalidateUses();
    plan.prepareRange(0, plan.size());
    auto first = resources(plan, 0, 2, RMResourceKind::Texture);
    auto second = resources(plan, 2, 4, RMResourceKind::Texture);
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0].access, read | sample);
    EXPECT_EQ(second[0].access, write);
    EXPECT_EQ(resources(plan, 0, 4, RMResourceKind::Texture)[0].access, read | write | sample);
}

}  // namespace
