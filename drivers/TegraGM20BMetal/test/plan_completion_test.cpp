// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../TegraGM20BMetalPlanExecution.h"
#include "../retirement.h"
#include <array>
#include <atomic>
#include <iterator>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

void *object(uintptr_t value)
{
    return reinterpret_cast<void *>(value);
}

void draw(SwitchMetalCommandPlan &plan)
{
    Cmd command = cmd(OpDraw, 3);
    command.b = 3;
    command.c = uint64_t(1) << 32;
    plan.record(command, {});
}

void event(SwitchMetalCommandPlan &plan, Op op, uintptr_t identity)
{
    Cmd command = cmd(op);
    command.obj = object(identity);
    command.c = identity + 20;
    plan.record(command, {});
}

TEST(PlanExecution, ClosedEncoderSummariesPreserveEvents)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    for (unsigned i = 0; i < 10000; ++i)
        draw(plan);
    plan.record(cmd(OpEnd), {});
    event(plan, OpSignalEvent, 7);
    event(plan, OpWaitEvent, 8);
    const size_t second = plan.size();
    plan.record(cmd(OpCompute), {});
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpBlit), {});
    plan.record(cmd(OpEnd), {});
    gm20b::ExecutionPlan result;
    ASSERT_EQ(makeExecutionPlan(plan, result), SwitchMetalPlanError::None);
    ASSERT_EQ(result.ranges().size(), 4u);
    EXPECT_EQ(result.ranges()[0].to, 10002u);
    EXPECT_EQ(result.ranges()[1].kind, gm20b::ExecutionKind::signal);
    EXPECT_EQ(result.ranges()[2].kind, gm20b::ExecutionKind::wait);
    EXPECT_TRUE(result.ranges()[2].gpuWait);
    EXPECT_EQ(result.ranges()[3].from, second);
    EXPECT_EQ(result.ranges()[3].to, plan.size());
    EXPECT_EQ(result.ranges()[3].kind, gm20b::ExecutionKind::native);
    EXPECT_EQ(plan.event(second - 2).event, object(7));
    EXPECT_EQ(plan.event(second - 1).value, 28u);
}

// A command recorded on an encoder that had ended lands outside every encoder: Metal's validation error, which the plan
// refuses whole
TEST(PlanExecution, ACommandAfterItsEncoderEndedIsAPlanError)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    plan.record(cmd(OpEnd), {});
    draw(plan);
    gm20b::ExecutionPlan result;
    EXPECT_EQ(makeExecutionPlan(plan, result), SwitchMetalPlanError::CommandAfterEnd);
}

// A copy or fill into `into` from `from`
void transfer(SwitchMetalCommandPlan &plan, Op op, uintptr_t from, uintptr_t into)
{
    Cmd command = cmd(op);
    command.obj = object(from);
    command.obj2 = object(into);
    plan.record(command, {});
}

using Run = std::array<size_t, 3>;

// Each run outside every encoder (the encoder it follows, from, to), each joined to that encoder as it is met
std::vector<Run> joinRuns(SwitchMetalCommandPlan &plan)
{
    std::vector<Run> runs;
    forEachLateRun(plan, [&](size_t encoder, size_t from, size_t to) {
        runs.push_back({encoder, from, to});
        if (encoder != SIZE_MAX)
            plan.joinEnded(encoder, to);
        return true;
    });
    return runs;
}

// An M4 Max with validation off runs a copy and a fill recorded on the blit encoder that ended last inside it, after
// its copy, before the next encoder (endprobe b_a_copy): the tape takes them before that encoder's end
TEST(PlanExecution, CommandsAfterTheLastEncoderEndedJoinItBeforeItsEnd)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    transfer(plan, OpCopyBuffer, 1, 9);
    plan.record(cmd(OpEnd), {});
    transfer(plan, OpCopyBuffer, 2, 9);
    transfer(plan, OpFillBuffer, 3, 9);
    plan.record(cmd(OpRender), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    gm20b::ExecutionPlan result;
    ASSERT_EQ(makeExecutionPlan(plan, result), SwitchMetalPlanError::CommandAfterEnd);
    EXPECT_EQ(joinRuns(plan), (std::vector<::Run>{{0, 3, 5}}));
    EXPECT_EQ(plan.encoderEnd(0), 5u);
    const Op ops[] = {OpBlit, OpCopyBuffer, OpCopyBuffer, OpFillBuffer, OpEnd, OpRender, OpDraw, OpEnd};
    ASSERT_EQ(plan.size(), std::size(ops));
    for (size_t at = 0; at < plan.size(); ++at)
        EXPECT_EQ(plan.operation(at).op, ops[at]) << at;
    EXPECT_EQ(plan.legacy(1).obj, object(1));
    EXPECT_EQ(plan.legacy(2).obj, object(2));
    EXPECT_EQ(plan.legacy(3).obj, object(3));
    EXPECT_EQ(plan.encoderEnd(5), plan.size());
    result.clear();
    ASSERT_EQ(makeExecutionPlan(plan, result), SwitchMetalPlanError::None);
    ASSERT_EQ(result.ranges().size(), 1u);
    EXPECT_EQ(result.ranges()[0].to, plan.size());
}

// Late draws join their pass's raster groups, a draw after a late pipeline change a group of its own after the pass's
// (endprobe r_a_draw, r_a_pipeline), while the pass begun after them keeps its own
TEST(PlanExecution, LateDrawsJoinTheirPassesRasterGroups)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    draw(plan);
    Cmd pipeline = cmd(OpPipeline);
    pipeline.obj = object(5);
    plan.record(pipeline, {});
    draw(plan);
    const size_t second = plan.size();
    plan.record(cmd(OpRender), {});
    plan.record(pipeline, {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    EXPECT_EQ(joinRuns(plan), (std::vector<::Run>{{0, 3, 5}}));
    const auto groups = [&](size_t begin) {
        std::vector<std::pair<const void *, uint32_t>> found;
        plan.forEachRasterGroup(begin, [&](const gm20b::RasterState &state, uint32_t draws) {
            found.push_back({state.pipeline, draws});
        });
        return found;
    };
    using Groups = std::vector<std::pair<const void *, uint32_t>>;
    EXPECT_EQ(groups(0), (Groups{{nullptr, 2}, {object(5), 1}}));
    EXPECT_EQ(groups(second), (Groups{{object(5), 1}}));
    gm20b::ExecutionPlan result;
    EXPECT_EQ(makeExecutionPlan(plan, result), SwitchMetalPlanError::None);
}

// A dispatch whose kernels are still being made, its pending record `pending`
void pendingDispatch(SwitchMetalCommandPlan &plan, uintptr_t pending)
{
    Cmd command = cmd(OpDispatch);
    command.obj = object(pending);
    command.c = UINT64_C(1) | UINT64_C(1) << 16 | UINT64_C(1) << 32;
    const uint64_t dimensions[] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    plan.record(command, dimensions);
}

// A dispatch on the compute encoder that ended last runs in it (endprobe c_a_dispatch): its range lists it where it now
// lies, before the encoder's end, for the commit's readiness to wait on its kernels
TEST(PlanExecution, ALateDispatchJoinsItsComputeEncodersRange)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    pendingDispatch(plan, 2);
    plan.record(cmd(OpEnd), {});
    pendingDispatch(plan, 3);
    EXPECT_EQ(joinRuns(plan), (std::vector<::Run>{{0, 3, 4}}));
    ASSERT_EQ(plan.encoderEnd(0), 4u);
    EXPECT_EQ(plan.operation(2).op, OpDispatch);
    EXPECT_EQ(plan.operation(3).op, OpEnd);
    EXPECT_EQ(plan.dispatch(2).pending, object(3));
    plan.prepareRange(0, 4);
    EXPECT_EQ(plan.pendingComputes(0, 4), (std::vector<size_t>{1, 2}));
}

// What an M4 Max crashes on (endprobe r_c_blit_open, b_c_render_ended, r_a_event_between): a render encoder's draw
// recorded inside the blit encoder begun after it ended is a recording error; a run after an event follows no encoder,
// and a copy is no command a render encoder records
TEST(PlanExecution, CommandsAfterOtherWorkBeganNameNoEncoderOfTheirs)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    gm20b::ExecutionPlan result;
    EXPECT_EQ(makeExecutionPlan(plan, result), SwitchMetalPlanError::CommandAfterOtherWork);
    plan.clear();
    plan.record(cmd(OpBlit), {});
    plan.record(cmd(OpEnd), {});
    event(plan, OpSignalEvent, 7);
    transfer(plan, OpCopyBuffer, 1, 9);
    EXPECT_EQ(joinRuns(plan), (std::vector<::Run>{{SIZE_MAX, 3, 4}}));
    EXPECT_FALSE(recordedBy(OpRender, OpCopyBuffer));
    EXPECT_FALSE(recordedBy(OpBlit, OpDraw));
    EXPECT_FALSE(recordedBy(OpBlit, OpDispatch));
    EXPECT_TRUE(recordedBy(OpCompute, OpDispatch));
    EXPECT_TRUE(recordedBy(OpBlit, OpWaitFence));
}

TEST(PlanExecution, RecordingErrorsCannotHideInsideSkippedEncoderBodies)
{
    for (Op invalid : {OpWaitEvent, OpSignalEvent, OpCompute}) {
        SwitchMetalCommandPlan plan;
        plan.record(cmd(OpRender), {});
        draw(plan);
        plan.record(cmd(invalid), {});
        plan.record(cmd(OpEnd), {});
        gm20b::ExecutionPlan result;
        EXPECT_EQ(makeExecutionPlan(plan, result), invalid == OpCompute ? SwitchMetalPlanError::NestedEncoder
                                                                        : SwitchMetalPlanError::EventInsideEncoder);
        EXPECT_TRUE(result.ranges().empty());
        plan.clear();
        plan.record(cmd(OpRender), {});
        EXPECT_EQ(makeExecutionPlan(plan, result), SwitchMetalPlanError::UnclosedEncoder);
        plan.record(cmd(OpEnd), {});
        EXPECT_EQ(makeExecutionPlan(plan, result), SwitchMetalPlanError::None);
    }
}

gm20b::RetirementWrites writes(const SwitchMetalCommandPlan &plan, size_t from, size_t to)
{
    return gm20b::RetirementWrites::capture(
        [&](auto visit) {
            plan.forEachWritten(from, to, visit);
        },
        [](void *value) {
            const auto identity = reinterpret_cast<uintptr_t>(value);
            if (identity >= 100 && identity <= 102)
                return gm20b::RetirementWrites::Target{object(9), value};
            return gm20b::RetirementWrites::Target{value, nullptr};
        });
}

TEST(PlanCompletion, AliasedTexturesCleanTheirParentBufferOnce)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.addUses(0, {{object(9), gm20b::ResourceUse::Write},
                     {object(100), gm20b::ResourceUse::Write},
                     {object(101), gm20b::ResourceUse::Write},
                     {object(102), gm20b::ResourceUse::Read},
                     {object(10), gm20b::ResourceUse::Write}});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    const auto snapshot = writes(plan, 0, plan.size());
    EXPECT_EQ(snapshot.buffers, (std::vector<void *>{object(9), object(10)}));
    EXPECT_EQ(snapshot.textures, (std::vector<void *>{object(100), object(101)}));
}

TEST(PlanCompletion, EarlierWritesRemainImmutableWhenLaterAnalysisChanges)
{
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    plan.addUses(0, {{object(100), gm20b::ResourceUse::Write}});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    const auto snapshot = std::make_shared<const gm20b::RetirementWrites>(writes(plan, 0, plan.size()));
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
        plan.addUses(0, {{object(200 + i), gm20b::ResourceUse::Write}});
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
    SwitchMetalCommandPlan plan;
    plan.record(cmd(OpBlit), {});
    plan.addUses(0, {{object(100), gm20b::ResourceUse::Read}});
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpBlit), {});
    plan.addUses(2, {{object(101), gm20b::ResourceUse::Write}});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, 2);
    plan.prepareRange(2, 4);
    EXPECT_TRUE(writes(plan, 0, 2).buffers.empty());
    EXPECT_EQ(writes(plan, 2, 4).textures, (std::vector<void *>{object(101)}));
}

}  // namespace
