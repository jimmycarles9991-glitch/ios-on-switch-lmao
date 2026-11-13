// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <algorithm>
#include <functional>
#include <vector>
#include "drivers/TegraGM20B/test/switch_gpu_event_fixture.h"
#include "drivers/TegraVIC/SwitchVICEvents.h"
#include "drivers/TegraVIC/SwitchVICSemantics.h"
#include "drivers/TegraHost1x/SwitchHost1xResults.h"

namespace {
switch_gpu_backing_semantics registered;
std::vector<switch_gpu_event_waits *> retired;
std::function<void(switch_gpu_event_waits *)> onRetired;

void recordRetired(switch_gpu_event_waits *batch)
{
    retired.push_back(batch);
    if (onRetired)
        onRetired(batch);
}

void ready(void *context, int status)
{
    auto &notice = *static_cast<Notices *>(context);
    notice.result = status;
    ++notice.count;
}
}

extern "C" int switch_gpu_backing_register_semantics(uint32_t engine, const switch_gpu_backing_semantics *adapter)
{
    EXPECT_EQ(engine, SWITCH_GPU_ENGINE_VIC);
    registered = *adapter;
    return 0;
}

TEST_F(Events, VicRequestWithoutPortsKeepsOrdinarySubmissionPath)
{
    TegraVICRequest request = {};
    SwitchVICEvents *work = nullptr;
    ASSERT_EQ(switch_vic_events_create(this, request, nullptr, nullptr, &work), 0);
    EXPECT_EQ(work, nullptr);
}

TEST_F(Events, VicKnownProducerRetainsHardwareDependencyAndFullWidthSignal)
{
    const uint64_t value = (1ull << 48) + 3;
    auto source = token(), consumer = token();
    signal(value, source);
    TegraVICRequest request = {};
    request.src_event_port = port;
    request.src_wait = value;
    request.src_signal = value + 1;
    auto &notices = makeNotices();
    SwitchVICEvents *work = nullptr;
    ASSERT_EQ(switch_vic_events_create(this, request, ready, &notices, &work), 0);
    switch_vic_events_arm(work);
    ASSERT_EQ(notices.count, 1);
    pump();
    ASSERT_EQ(notices.result, 0);
    ASSERT_EQ(switch_vic_events_prepare(work), 0);
    const switch_gpu_backing_token *dependencies = nullptr;
    ASSERT_EQ(switch_vic_events_tokens(work, &dependencies), 1u);
    EXPECT_EQ(key(dependencies[0]), key(source));
    auto *batch = switch_vic_events_batch(work);
    ASSERT_EQ(switch_gpu_event_waits_bind(batch, &consumer), 0);
    ASSERT_EQ(switch_vic_events_bind(work, &consumer), 0);
    commit(batch);
    switch_vic_events_commit(work);
    finish(consumer);
    EXPECT_EQ(native->value, 0u);
    finish(source);
    EXPECT_EQ(native->value, value + 1);
    switch_vic_events_release(&work);
    pump();
}

TEST_F(Events, VicUnpublishedCpuProducerUsesOneShotNotification)
{
    TegraVICRequest request = {};
    request.src_event_port = port;
    request.src_wait = 1ull << 40;
    auto &notices = makeNotices();
    SwitchVICEvents *work = nullptr;
    ASSERT_EQ(switch_vic_events_create(this, request, ready, &notices, &work), 0);
    switch_vic_events_arm(work);
    pump();
    EXPECT_EQ(notices.count, 0);
    switch_gpu_event_native_signal(native, request.src_wait, false);
    nativePump();
    EXPECT_EQ(notices.count, 1);
    ASSERT_EQ(switch_vic_events_prepare(work), 0);
    const switch_gpu_backing_token *dependencies = nullptr;
    EXPECT_EQ(switch_vic_events_tokens(work, &dependencies), 0u);
    switch_vic_events_release(&work);
    pump();
}

TEST_F(Events, VicWaitsForBothRequestEventsWithoutDuplicateNotification)
{
    TegraVICRequest request = {};
    request.src_event_port = request.dst_event_port = port;
    request.src_wait = 4;
    request.dst_wait = 8;
    auto &notices = makeNotices();
    SwitchVICEvents *work = nullptr;
    ASSERT_EQ(switch_vic_events_create(this, request, ready, &notices, &work), 0);
    switch_vic_events_arm(work);
    signal(4, token());
    pump();
    EXPECT_EQ(notices.count, 0);
    signal(8, token());
    pump();
    EXPECT_EQ(notices.count, 1);
    ASSERT_EQ(switch_vic_events_prepare(work), 0);
    const switch_gpu_backing_token *dependencies = nullptr;
    EXPECT_EQ(switch_vic_events_tokens(work, &dependencies), 2u);
    switch_vic_events_release(&work);
    pump();
}

TEST_F(Events, VicCancellationRemovesNativePendingWaitAndNeverCallsReleasedContext)
{
    TegraVICRequest request = {};
    request.src_event_port = port;
    request.src_wait = 7;
    auto &notices = makeNotices();
    SwitchVICEvents *work = nullptr;
    ASSERT_EQ(switch_vic_events_create(this, request, ready, &notices, &work), 0);
    switch_vic_events_arm(work);
    switch_vic_events_release(&work);
    switch_gpu_event_native_signal(native, 7, false);
    nativePump();
    EXPECT_EQ(notices.count, 0);
    EXPECT_TRUE(pending.empty());
}

TEST_F(Events, VicProducerFailureDoesNotPublishSuccessfulNativeSignal)
{
    auto source = token(), consumer = token();
    signal(3, source);
    TegraVICRequest request = {};
    request.src_event_port = port;
    request.src_wait = 3;
    request.src_signal = 4;
    auto &notices = makeNotices();
    SwitchVICEvents *work = nullptr;
    ASSERT_EQ(switch_vic_events_create(this, request, ready, &notices, &work), 0);
    switch_vic_events_arm(work);
    pump();
    ASSERT_EQ(switch_vic_events_prepare(work), 0);
    auto *batch = switch_vic_events_batch(work);
    ASSERT_EQ(switch_gpu_event_waits_bind(batch, &consumer), 0);
    ASSERT_EQ(switch_vic_events_bind(work, &consumer), 0);
    commit(batch);
    switch_vic_events_commit(work);
    finish(consumer);
    finish(source, -5);
    EXPECT_EQ(switch_gpu_event_waits_status(batch), -5);
    EXPECT_EQ(native->value, 0u);
    switch_vic_events_release(&work);
    pump();
}

TEST_F(Events, VicSemanticAdapterKeepsIndependentJobOutcomes)
{
    ASSERT_TRUE(switch_vic_semantics_start(161, recordRetired));
    switch_gpu_backing_token first{1, 1, SWITCH_GPU_ENGINE_VIC, 161};
    switch_gpu_backing_token second{2, 1, SWITCH_GPU_ENGINE_VIC, 161};
    using Results = SwitchHost1xResults<SWITCH_GPU_ENGINE_VIC>;
    auto *ra = Results::allocate(), *rb = Results::allocate();
    ASSERT_EQ(Results::prepare(ra, 1, 1, &first), 0);
    ASSERT_EQ(Results::prepare(rb, 2, 2, &second), 0);
    tokens[key(first)] = {};
    tokens[key(second)] = {};
    auto *a = batch(nullptr, first), *b = batch(nullptr, second);
    SwitchVICSemantic *node = nullptr;
    ASSERT_EQ(switch_vic_semantics_bind(a, &first, &node), 0);
    switch_vic_semantics_commit(node);
    commit(a);
    ASSERT_EQ(switch_vic_semantics_bind(b, &second, &node), 0);
    switch_vic_semantics_commit(node);
    commit(b);
    switch_gpu_event_waits *captured = nullptr;
    ASSERT_EQ(registered.snapshot(&second, &captured), 1);
    EXPECT_EQ(captured, b);
    switch_gpu_event_waits_release(&captured);
    finish(second);
    EXPECT_EQ(switch_gpu_event_waits_status(b), 1);
    Results::physical(rb, 1);
    switch_vic_semantics_physical(second.value);
    EXPECT_EQ(registered.physical_status(&second, 0), 1);
    EXPECT_TRUE(registered.pending());
    finish(first);
    EXPECT_EQ(switch_gpu_event_waits_status(b), 1);
    EXPECT_EQ(std::count(retired.begin(), retired.end(), a), 1);
    EXPECT_EQ(std::count(retired.begin(), retired.end(), b), 1);
    EXPECT_FALSE(registered.pending());
    EXPECT_EQ(registered.snapshot(&second, &captured), 0);
    Results::complete(&ra, 0);
    Results::complete(&rb, 0);
    switch_gpu_event_waits_release(&a);
    switch_gpu_event_waits_release(&b);
    pump();
}

TEST_F(Events, IntermediateVicStageWaitsWithoutPublishingFinalRequestSignal)
{
    auto source = token(), vic = token(), copyback = token();
    signal(3, source);
    TegraVICRequest request = {};
    request.src_event_port = port;
    request.src_wait = 3;
    request.src_signal = 4;
    auto &notice = makeNotices();
    SwitchVICEvents *work = nullptr, *stage = nullptr;
    ASSERT_EQ(switch_vic_events_create(this, request, ready, &notice, &work), 0);
    switch_vic_events_arm(work);
    ASSERT_EQ(switch_vic_events_prepare(work), 0);
    ASSERT_EQ(switch_vic_events_stage(work, &stage), 0);
    auto *intermediate = switch_vic_events_batch(stage);
    ASSERT_EQ(switch_gpu_event_waits_bind(intermediate, &vic), 0);
    ASSERT_EQ(switch_vic_events_bind(stage, &vic), 0);
    commit(intermediate);
    switch_vic_events_commit(stage);
    finish(vic);
    EXPECT_EQ(native->value, 0u);
    finish(source);
    EXPECT_EQ(native->value, 3u);
    switch_vic_events_release(&stage);
    switch_gpu_event_submission submission = {};
    switch_vic_events_submission(work, true, &submission);
    ASSERT_EQ(submission.signal_count, 1u);
    auto *final = batch(nullptr, copyback);
    ASSERT_EQ(switch_gpu_event_waits_merge_uncommitted(final, submission.waits), 0);
    ASSERT_EQ(switch_gpu_event_signal_inherit(submission.signals[0], final), 0);
    ASSERT_EQ(switch_gpu_event_signal_bind(submission.signals[0], &copyback), 0);
    commit(final);
    switch_gpu_event_signal_commit(&submission.signals[0]);
    switch_vic_events_commit(work);
    finish(copyback);
    EXPECT_EQ(native->value, 4u);
    switch_gpu_event_waits_release(&final);
    switch_vic_events_release(&work);
    pump();
}

// A VIC job ends from the events' retirement when its semantic outcome comes last (SwitchVICJobs.cpp ended()): its
// batch reference and its request's events are released there, with the events' lock dropped and no thread of VIC's.
TEST_F(Events, AVicJobEndingInTheEventsRetirementReleasesItsRequestThere)
{
    ASSERT_TRUE(switch_vic_semantics_start(161, recordRetired));
    auto source = token();
    signal(3, source);
    TegraVICRequest request = {};
    request.src_event_port = port;
    request.src_wait = 3;
    request.src_signal = 4;
    auto &notices = makeNotices();
    SwitchVICEvents *work = nullptr;
    ASSERT_EQ(switch_vic_events_create(this, request, ready, &notices, &work), 0);
    switch_vic_events_arm(work);
    pump();
    ASSERT_EQ(switch_vic_events_prepare(work), 0);
    using Results = SwitchHost1xResults<SWITCH_GPU_ENGINE_VIC>;
    switch_gpu_backing_token consumer{};
    auto *record = Results::allocate();
    ASSERT_EQ(Results::prepare(record, 40, 40, &consumer), 0);
    tokens[key(consumer)] = {};
    auto *batch = switch_vic_events_batch(work);
    switch_gpu_event_waits_retain(batch);
    SwitchVICSemantic *node = nullptr;
    ASSERT_EQ(switch_vic_semantics_bind(batch, &consumer, &node), 0);
    ASSERT_EQ(switch_vic_events_bind(work, &consumer), 0);
    switch_vic_semantics_commit(node);
    commit(batch);
    switch_vic_events_commit(work);
    int ended = 0;
    onRetired = [&](switch_gpu_event_waits *done) {
        EXPECT_EQ(done, batch);
        switch_gpu_event_waits_release(&done);
        switch_vic_events_release(&work);
        ++ended;
    };
    Results::physical(record, 1);
    switch_vic_semantics_physical(40);
    finish(consumer);
    finish(source);
    pump();
    onRetired = nullptr;
    EXPECT_EQ(ended, 1);
    EXPECT_EQ(work, nullptr);
    EXPECT_EQ(native->value, 4u);
    Results::complete(&record, 0);
    pump();
}
