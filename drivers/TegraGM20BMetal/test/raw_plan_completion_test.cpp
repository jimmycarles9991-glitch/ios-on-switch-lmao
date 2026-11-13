// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/RawMetalPlanExecution.h"
#include "../raw/runtime/raw_retirement.h"
#include <atomic>
#include <memory>
#include <thread>

namespace {

void *object(uintptr_t value)
{
    return reinterpret_cast<void *>(value);
}

void draw(RMCommandPlan &plan)
{
    Cmd command = cmd(OpDraw, 3);
    command.b = 3;
    command.c = uint64_t(1) << 32;
    plan.record(command, {});
}

void event(RMCommandPlan &plan, Op op, uintptr_t identity)
{
    Cmd command = cmd(op);
    command.obj = object(identity);
    command.c = identity + 20;
    plan.record(command, {});
}

TEST(PlanExecution, ClosedEncoderSummariesPreserveEventsAndLateRefusals)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    for (unsigned i = 0; i < 10000; ++i)
        draw(plan);
    plan.record(cmd(OpEnd), {});
    event(plan, OpSignalEvent, 7);
    event(plan, OpWaitEvent, 8);
    const size_t second = plan.size();
    plan.record(cmd(OpCompute), {});
    plan.record(cmd(OpEnd), {});
    const size_t third = plan.size();
    plan.record(cmd(OpBlit), {});
    plan.record(cmd(OpEnd), {});
    plan.setRefused(third, true);
    plan.record(cmd(OpInvocation), {});
    raw::ExecutionPlan result;
    std::vector<RMPlanEvent> observed;
    ASSERT_EQ(makeExecutionPlan(plan, false, result,
                                [&](const auto &value) {
                                    observed.push_back(value);
                                    return true;
                                }),
              RMPlanError::None);
    ASSERT_EQ(result.ranges().size(), 5u);
    EXPECT_EQ(result.ranges()[0].to, 10002u);
    EXPECT_EQ(result.ranges()[1].kind, raw::ExecutionKind::signal);
    EXPECT_EQ(result.ranges()[2].kind, raw::ExecutionKind::wait);
    EXPECT_TRUE(result.ranges()[2].gpuWait);
    EXPECT_EQ(result.ranges()[3].from, second);
    EXPECT_EQ(result.ranges()[3].to, third);
    EXPECT_EQ(result.ranges()[4].kind, raw::ExecutionKind::compatibility);
    EXPECT_EQ(result.ranges()[4].to, plan.size());
    ASSERT_EQ(observed.size(), 2u);
    EXPECT_EQ(observed[0].event, object(7));
    EXPECT_EQ(observed[1].value, 28u);
}

TEST(PlanExecution, WholeCompatibilityAndOpaqueEventsKeepTheirPositions)
{
    RMCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    plan.record(cmd(OpEnd), {});
    event(plan, OpWaitEvent, 3);
    plan.record(cmd(OpCompute), {});
    plan.record(cmd(OpEnd), {});
    raw::ExecutionPlan result;
    ASSERT_EQ(makeExecutionPlan(plan, true, result,
                                [](const auto &) {
                                    return false;
                                }),
              RMPlanError::None);
    ASSERT_EQ(result.ranges().size(), 1u);
    EXPECT_EQ(result.ranges()[0].kind, raw::ExecutionKind::compatibility);
    EXPECT_EQ(result.ranges()[0].from, 0u);
    EXPECT_EQ(result.ranges()[0].to, plan.size());
}

TEST(PlanExecution, RecordingErrorsCannotHideInsideSkippedEncoderBodies)
{
    for (Op invalid : {OpWaitEvent, OpSignalEvent, OpCompute}) {
        RMCommandPlan plan;
        plan.record(cmd(OpRender), {});
        draw(plan);
        plan.record(cmd(invalid), {});
        plan.record(cmd(OpEnd), {});
        raw::ExecutionPlan result;
        unsigned callbacks = 0;
        EXPECT_EQ(makeExecutionPlan(plan, false, result,
                                    [&](const auto &) {
                                        ++callbacks;
                                        return true;
                                    }),
                  invalid == OpCompute ? RMPlanError::NestedEncoder : RMPlanError::EventInsideEncoder);
        EXPECT_TRUE(result.ranges().empty());
        EXPECT_EQ(callbacks, 0u);
        plan.clear();
        plan.record(cmd(OpRender), {});
        EXPECT_EQ(makeExecutionPlan(plan, false, result,
                                    [](const auto &) {
                                        return true;
                                    }),
                  RMPlanError::UnclosedEncoder);
        plan.record(cmd(OpEnd), {});
        EXPECT_EQ(makeExecutionPlan(plan, false, result,
                                    [](const auto &) {
                                        return true;
                                    }),
                  RMPlanError::None);
    }
}

raw::RetirementWrites writes(const RMCommandPlan &plan, size_t from, size_t to)
{
    return raw::RetirementWrites::capture(
        [&](auto visit) {
            plan.forEachWritten(from, to, visit);
        },
        [](void *value) {
            const auto identity = reinterpret_cast<uintptr_t>(value);
            if (identity >= 100 && identity <= 102)
                return raw::RetirementWrites::Target{object(9), value};
            return raw::RetirementWrites::Target{value, nullptr};
        });
}

TEST(PlanCompletion, AliasedTexturesCleanTheirParentBufferOnce)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.addUses(0, {{object(9), raw::ResourceUse::Write},
                     {object(100), raw::ResourceUse::Write},
                     {object(101), raw::ResourceUse::Write},
                     {object(102), raw::ResourceUse::Read},
                     {object(10), raw::ResourceUse::Write}});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    const auto snapshot = writes(plan, 0, plan.size());
    EXPECT_EQ(snapshot.buffers, (std::vector<void *>{object(9), object(10)}));
    EXPECT_EQ(snapshot.textures, (std::vector<void *>{object(100), object(101)}));
}

TEST(PlanCompletion, EarlierWritesRemainImmutableWhenLaterAnalysisChanges)
{
    RMCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    plan.addUses(0, {{object(100), raw::ResourceUse::Write}});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    const auto snapshot = std::make_shared<const raw::RetirementWrites>(writes(plan, 0, plan.size()));
    std::atomic<bool> started{false}, invalid{false};
    std::thread completion([&] {
        started.store(true, std::memory_order_release);
        for (unsigned i = 0; i < 10000; ++i)
            if (snapshot->buffers != std::vector<void *>{object(9)} ||
                snapshot->textures != std::vector<void *>{object(100)})
                invalid.store(true, std::memory_order_relaxed);
    });
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    for (unsigned i = 0; i < 2000; ++i) {
        plan.clear();
        plan.record(cmd(OpBlit), {});
        plan.addUses(0, {{object(200 + i), raw::ResourceUse::Write}});
        plan.record(cmd(OpEnd), {});
        plan.invalidateUses();
        plan.prepareRange(0, plan.size());
    }
    completion.join();
    EXPECT_FALSE(invalid.load());
    EXPECT_NE(writes(plan, 0, plan.size()).buffers, snapshot->buffers);
}

TEST(PlanCompletion, DistinctRangesAndEmptyWritesRemainSeparate)
{
    RMCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    plan.addUses(0, {{object(100), raw::ResourceUse::Read}});
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpBlit), {});
    plan.addUses(2, {{object(101), raw::ResourceUse::Write}});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, 2);
    plan.prepareRange(2, 4);
    EXPECT_TRUE(writes(plan, 0, 2).buffers.empty());
    EXPECT_EQ(writes(plan, 2, 4).textures, (std::vector<void *>{object(101)}));
}

}  // namespace
