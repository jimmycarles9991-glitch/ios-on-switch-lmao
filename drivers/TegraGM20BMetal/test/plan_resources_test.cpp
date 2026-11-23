// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../TegraGM20BMetalPlanExecution.h"

namespace {

void *object(uintptr_t value)
{
    return reinterpret_cast<void *>(value);
}

constexpr uint8_t read = gm20b::ResourceUse::Read;
constexpr uint8_t write = gm20b::ResourceUse::Write;
constexpr uint8_t sample = gm20b::ResourceUse::Sample;

void resource(SwitchMetalCommandPlan &plan, uintptr_t value, SwitchMetalResourceKind kind, uint8_t roles = 0,
              uintptr_t buffer = 0)
{
    plan.registerResource(object(value), {object(buffer), kind, roles});
}

size_t pass(SwitchMetalCommandPlan &plan, const std::vector<gm20b::ResourceUse> &uses)
{
    const auto at = plan.size();
    plan.record(cmd(OpRender), {});
    plan.addUses(at, uses);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(at, plan.size());
    return at;
}

std::vector<SwitchMetalRangeResource> resources(const SwitchMetalCommandPlan &plan, size_t from, size_t to,
                                                SwitchMetalResourceKind kind)
{
    std::vector<SwitchMetalRangeResource> result;
    plan.forEachResource(from, to, kind, [&](const auto &use) {
        result.push_back(use);
    });
    return result;
}

TEST(PlanResources, OneMergedResourceCarriesItsKindAliasAndExactAccess)
{
    SwitchMetalCommandPlan plan;
    resource(plan, 1, SwitchMetalResourceKind::Texture, SwitchMetalResourceFacts::Converted, 2);
    resource(plan, 2, SwitchMetalResourceKind::Buffer, 0, 2);
    resource(plan, 3, SwitchMetalResourceKind::Sampler);
    pass(plan,
         {{object(1), read | sample}, {object(2), read}, {object(1), write}, {object(3), read}, {object(4), read}});
    const auto textures = resources(plan, 0, plan.size(), SwitchMetalResourceKind::Texture);
    ASSERT_EQ(textures.size(), 1u);
    EXPECT_EQ(textures[0].object, object(1));
    EXPECT_EQ(textures[0].facts.cpuBuffer, object(2));
    EXPECT_EQ(textures[0].facts.roles, SwitchMetalResourceFacts::Converted);
    EXPECT_EQ(textures[0].access, read | write | sample);
    EXPECT_EQ(resources(plan, 0, plan.size(), SwitchMetalResourceKind::Buffer).size(), 1u);
    EXPECT_EQ(resources(plan, 0, plan.size(), SwitchMetalResourceKind::Sampler).size(), 1u);
    const auto others = resources(plan, 0, plan.size(), SwitchMetalResourceKind::Other);
    ASSERT_EQ(others.size(), 1u);
    EXPECT_EQ(others[0].object, object(4));
}

TEST(PlanResources, NewRetainedObjectsPreserveCompletedEncoderSummaries)
{
    SwitchMetalCommandPlan plan;
    resource(plan, 1, SwitchMetalResourceKind::Buffer, 0, 1);
    pass(plan, {{object(1), read}});
    const SwitchMetalRangeResource *first = nullptr;
    plan.forEachResource(0, 2, SwitchMetalResourceKind::Buffer, [&](const auto &use) {
        first = &use;
    });
    resource(plan, 2, SwitchMetalResourceKind::Texture);
    pass(plan, {{object(2), write}});
    plan.prepareRange(0, 2);
    plan.forEachResource(0, 2, SwitchMetalResourceKind::Buffer, [&](const auto &use) {
        EXPECT_EQ(&use, first);
    });
    plan.prepareRange(0, plan.size());
    EXPECT_EQ(resources(plan, 0, plan.size(), SwitchMetalResourceKind::Buffer).size(), 1u);
    EXPECT_EQ(resources(plan, 0, plan.size(), SwitchMetalResourceKind::Texture).size(), 1u);
}

TEST(PlanResources, RegistrationIsImmutableUntilTheRecordingIsRecycled)
{
    SwitchMetalCommandPlan plan;
    resource(plan, 1, SwitchMetalResourceKind::Texture, SwitchMetalResourceFacts::Converted, 2);
    resource(plan, 1, SwitchMetalResourceKind::Buffer, 0, 3);
    pass(plan, {{object(1), read}});
    auto use = resources(plan, 0, plan.size(), SwitchMetalResourceKind::Texture);
    ASSERT_EQ(use.size(), 1u);
    EXPECT_EQ(use[0].facts.cpuBuffer, object(2));
    plan.clear();
    resource(plan, 1, SwitchMetalResourceKind::Buffer, 0, 3);
    pass(plan, {{object(1), read}});
    use = resources(plan, 0, plan.size(), SwitchMetalResourceKind::Buffer);
    ASSERT_EQ(use.size(), 1u);
    EXPECT_EQ(use[0].facts.cpuBuffer, object(3));
    EXPECT_TRUE(resources(plan, 0, plan.size(), SwitchMetalResourceKind::Texture).empty());
}

TEST(PlanResources, APlanarTargetWrittenAndSampledInOneEncoderStaysNative)
{
    SwitchMetalCommandPlan plan;
    resource(plan, 1, SwitchMetalResourceKind::Texture, SwitchMetalResourceFacts::Converted);
    pass(plan, {{object(1), write}, {object(1), read | sample}});
    gm20b::ExecutionPlan execution;
    EXPECT_EQ(makeExecutionPlan(plan, execution), SwitchMetalPlanError::None);
    ASSERT_EQ(execution.ranges().size(), 1u);
    EXPECT_EQ(execution.ranges()[0].kind, gm20b::ExecutionKind::native);
    EXPECT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan.legacy(0).op, OpRender);
    EXPECT_EQ(plan.legacy(1).op, OpEnd);
}

TEST(PlanResources, APlanarTargetWrittenThenSampledIsOneNativeRange)
{
    SwitchMetalCommandPlan plan;
    resource(plan, 1, SwitchMetalResourceKind::Texture, SwitchMetalResourceFacts::Converted);
    pass(plan, {{object(1), write}});
    pass(plan, {{object(1), read | sample}});
    gm20b::ExecutionPlan execution;
    ASSERT_EQ(makeExecutionPlan(plan, execution), SwitchMetalPlanError::None);
    ASSERT_EQ(execution.ranges().size(), 1u);
    EXPECT_EQ(execution.ranges()[0].kind, gm20b::ExecutionKind::native);
    EXPECT_EQ(execution.ranges()[0].to, 4u);
}

TEST(PlanResources, EventsSeparateConversionEpochs)
{
    SwitchMetalCommandPlan plan;
    resource(plan, 1, SwitchMetalResourceKind::Texture, SwitchMetalResourceFacts::Converted);
    pass(plan, {{object(1), write}});
    Cmd event = cmd(OpSignalEvent);
    event.obj = object(2);
    event.c = UINT64_C(0x100000001);
    plan.record(event, {});
    pass(plan, {{object(1), read | sample}});
    gm20b::ExecutionPlan execution;
    ASSERT_EQ(makeExecutionPlan(plan, execution), SwitchMetalPlanError::None);
    ASSERT_EQ(execution.ranges().size(), 3u);
    EXPECT_EQ(execution.ranges()[1].kind, gm20b::ExecutionKind::signal);
    EXPECT_EQ(plan.event(2).value, event.c);
}

TEST(PlanResources, APendingKernelsUsesAreItsAdmittedOnesOncePreparedAgain)
{
    SwitchMetalCommandPlan plan;
    resource(plan, 1, SwitchMetalResourceKind::Texture, SwitchMetalResourceFacts::Converted);
    plan.record(cmd(OpCompute), {});
    Cmd bind = cmd(OpKernelTexture, 3, 0);
    bind.obj = object(1);
    plan.record(bind, {});
    Cmd dispatch = cmd(OpDispatch);
    dispatch.obj = object(2);
    dispatch.c = UINT64_C(1) | UINT64_C(1) << 16 | UINT64_C(1) << 32;
    const uint64_t dimensions[] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    plan.record(dispatch, dimensions);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, 3);
    plan.setKernelUses(1, object(3), [](std::vector<gm20b::ResourceUse> &out) {
        out = std::vector<gm20b::ResourceUse>{{object(1), read | sample}};
    });
    plan.resolveDispatch(1, object(3), 0);
    plan.invalidateUses();
    plan.prepareRange(0, 3);
    const auto use = resources(plan, 0, 3, SwitchMetalResourceKind::Texture);
    ASSERT_EQ(use.size(), 1u);
    EXPECT_EQ(use[0].access, read | sample);
    plan.setKernelUses(1, object(4), [](std::vector<gm20b::ResourceUse> &out) {
        out = std::vector<gm20b::ResourceUse>{{object(1), read | write | sample}};
    });
    plan.invalidateUses();
    plan.prepareRange(0, 3);
    EXPECT_EQ(resources(plan, 0, 3, SwitchMetalResourceKind::Texture)[0].access, read | write | sample);
}

TEST(PlanResources, RebuildingACombinedRangeAlsoRestoresPerEncoderTypedResources)
{
    SwitchMetalCommandPlan plan;
    resource(plan, 1, SwitchMetalResourceKind::Texture);
    pass(plan, {{object(1), read | sample}});
    pass(plan, {{object(1), write}});
    plan.invalidateUses();
    plan.prepareRange(0, plan.size());
    auto first = resources(plan, 0, 2, SwitchMetalResourceKind::Texture);
    auto second = resources(plan, 2, 4, SwitchMetalResourceKind::Texture);
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0].access, read | sample);
    EXPECT_EQ(second[0].access, write);
    EXPECT_EQ(resources(plan, 0, 4, SwitchMetalResourceKind::Texture)[0].access, read | write | sample);
}

}  // namespace
