// SPDX-License-Identifier: MIT
// A command buffer held on a shared event nobody signals, on the facade over the kernel model, against what Metal
// does on an M4 Max: the untimed wait holds its queue's head for 5 s, then the command buffer fails with
// MTLCommandBufferErrorTimeout (IOGPU's error 2 under it), presents nothing and withdraws its writer promise, and the
// command buffers behind it run on in order. The kernel holds the same bound on its writer promises (method 34 Arm), so
// a reader goes on at it even when the holder cannot run. The test holds the event clock
// (switchMetalProbeAdvanceEventClock:) and moves the kernel model's own: no test sleeps, and no real time reaches what
// it measures.
#include "facade_drawn.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace drawn;

// The event clock held and moved on by `ms` (switchMetalProbeAdvanceEventClock:), then synchronized twice with the
// submit worker so applyChanges(), waiting.expire() and any expired head's turn have all run
void advanceEventClock(id<MTLDevice> device, id<MTLCommandQueue> other, long long ms)
{
    (void)other;
    ((id (*)(id, SEL, id))objc_msgSend)(device, sel_registerName("switchMetalProbeAdvanceEventClock:"), @(ms));
    gm20bRunOnSubmitWorker([] {});
    gm20bRunOnSubmitWorker([] {});
}

// A command buffer that signals `done` (in `order`) as it completes, committed
id<MTLCommandBuffer> committed(id<MTLCommandBuffer> cb, dispatch_semaphore_t done, std::vector<int> *order, int tag)
{
    static std::mutex lock;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        {
            std::lock_guard<std::mutex> l(lock);
            order->push_back(tag);
        }
        dispatch_semaphore_signal(done);
    }];
    [cb commit];
    return cb;
}

bool within(dispatch_semaphore_t done, int64_t seconds)
{
    return !dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, seconds * int64_t(NSEC_PER_SEC)));
}

// Signalled at each Make the kernel model takes (method 34), as a command buffer is held
dispatch_semaphore_t countMakes()
{
    dispatch_semaphore_t made = dispatch_semaphore_create(0);
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    kernel_model::model.promiseCalled = [made] {
        if (kernel_model::model.promiseCalls.back().op == TegraGM20BPromiseMake)
            dispatch_semaphore_signal(made);
    };
    return made;
}

// The kernel model's clock, which holders' bounds run on (TegraGM20BPromiseArm), moved on by `ms`
void advanceKernelClock(uint64_t ms)
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    kernel_model::model.advancePromiseClock(ms);
}

// The promise's word reached its value: a reader parked on it goes on
bool released(uint32_t promise)
{
    kernel_model::Model &m = kernel_model::model;
    return int32_t(m.promiseWord(promise) - m.promiseValues[promise]) >= 0;
}

// The bound of the last Arm that named `promise`, in ms; -1 when none did
long long armedMs(uint32_t promise)
{
    long long ms = -1;
    for (const auto &call : kernel_model::model.promiseCalls)
        if (call.op == TegraGM20BPromiseArm && (call.promise & (1u << promise)))
            ms = (long long)(call.ns / 1000000);
    return ms;
}

// Metal's GPU fails a command buffer whose untimed wait on a shared event nobody signals holds its queue's head for
// 5 s (an M4 Max: MTLCommandBufferErrorTimeout with IOGPU's error 2 under it; the buffers behind it run on, in order).
// The event clock is held from before the commit and moved instead of waited for: 4999 ms in it still waits; at 5000 ms
// it fails so (with a draw before its wait too, already submitted, as in Apple's measurement), its promise is
// withdrawn, its drawable not presented, the two command buffers behind it complete with no error, and the failure is
// counted under failed.event.timeout.
void heldCommandBufferFailsAtMetalsBound(bool drawFirst)
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue], other = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> elsewhere = [o.device newTextureWithDescriptor:td];
    id<MTLSharedEvent> event = [o.device newSharedEvent];
    kernel_model::Model &m = kernel_model::model;
    CountingDrawable *drawable = [CountingDrawable new];
    const kernel_model::HooksCleared cleared;
    advanceEventClock(o.device, other, 0);
    dispatch_semaphore_t made = countMakes();
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    std::vector<int> order;
    id<MTLCommandBuffer> held = [queue commandBuffer];
    if (drawFirst)
        drawInto(held, frame, o.state);
    [held encodeWaitForEvent:event value:1];
    drawInto(held, frame, o.state);
    [held presentDrawable:drawable];
    committed(held, done, &order, 0);
    ASSERT_TRUE(within(made, 10)) << "never held";
    advanceEventClock(o.device, other, 0);
    ASSERT_EQ(m.promiseCalls.size(), 2u) << "its write promised, and the promise armed with its bound";
    const uint32_t promise = m.promiseCalls[0].promise;
    EXPECT_EQ(armedMs(promise), 5000);
    for (int tag : {1, 2}) {
        id<MTLCommandBuffer> behind = [queue commandBuffer];
        drawInto(behind, elsewhere, o.state);
        committed(behind, done, &order, tag);
    }
    advanceEventClock(o.device, other, 4999);
    EXPECT_NE(held.status, MTLCommandBufferStatusError) << "failed before Metal's bound";
    EXPECT_EQ(m.promiseCalls.size(), 2u);
    advanceEventClock(o.device, other, 1);
    for (int i = 0; i < 3; i++)
        ASSERT_TRUE(within(done, 10)) << "command buffer " << i << " never completed";
    EXPECT_EQ(held.status, MTLCommandBufferStatusError);
    EXPECT_EQ(std::string(held.error.domain.UTF8String), std::string(MTLCommandBufferErrorDomain.UTF8String));
    EXPECT_EQ(held.error.code, MTLCommandBufferErrorTimeout);
    EXPECT_EQ(text(held.error.localizedDescription),
              "Caused GPU Timeout Error (00000002:kIOGPUCommandBufferCallbackErrorTimeout)");
    NSError *underlying = held.error.userInfo[NSUnderlyingErrorKey];
    EXPECT_EQ(text(underlying.domain), "IOGPUCommandQueueErrorDomain");
    EXPECT_EQ(underlying.code, 2);
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
    ASSERT_EQ(m.promiseCalls.size(), 3u);
    EXPECT_EQ(m.promiseCalls[2].op, uint32_t(TegraGM20BPromiseWithdraw));
    EXPECT_EQ(m.promiseCalls[2].promise, promise);
    EXPECT_EQ(drawable.presents, 0);
    EXPECT_EQ([counters(o.device)[@"failed.event.timeout"] longLongValue], 1);
}

// Signalled 4999 ms in, the held command buffer completes with no error and its promise is kept by its job, never
// withdrawn; signalled and then moved to the bound before the worker looks, the value reached wins; the clock moved far
// past the bound afterwards fails nothing that ran
void heldCommandBufferSignalledInTimeCompletes()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue], other = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);
    kernel_model::Model &m = kernel_model::model;
    const kernel_model::HooksCleared cleared;
    advanceEventClock(o.device, other, 0);
    dispatch_semaphore_t made = countMakes();
    for (const long long beforeSignal : {4999ll, 0ll}) {
        SCOPED_TRACE(beforeSignal);
        id<MTLSharedEvent> event = [o.device newSharedEvent];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        std::vector<int> order;
        const size_t calls = m.promiseCalls.size();
        id<MTLCommandBuffer> held = [queue commandBuffer];
        [held encodeWaitForEvent:event value:1];
        drawInto(held, frame, o.state);
        committed(held, done, &order, 0);
        ASSERT_TRUE(within(made, 10)) << "never held";
        if (beforeSignal)
            advanceEventClock(o.device, other, beforeSignal);
        event.signaledValue = 1;
        if (!beforeSignal)
            advanceEventClock(o.device, other, 5000);
        ASSERT_TRUE(within(done, 10));
        EXPECT_EQ(held.status, MTLCommandBufferStatusCompleted) << text(held.error);
        for (size_t i = calls; i < m.promiseCalls.size(); i++)
            EXPECT_NE(m.promiseCalls[i].op, uint32_t(TegraGM20BPromiseWithdraw)) << "a promise kept in time withdrawn";
    }
    advanceEventClock(o.device, other, 60000);
    EXPECT_EQ(jobsOfDrawn(queue, frame, o.state, nil) > 0, true);
    EXPECT_EQ([counters(o.device)[@"failed.event.timeout"] longLongValue], 0);
}

// A holder that cannot run: its event clock held, so its submit worker never reaches its own bound, as for an app iOS
// suspended mid-hold. Its promises are given up at the bound all the same, since the kernel holds it (method 34 Arm), as
// Metal's bound is its kernel's: the head's at 5000 ms on the kernel's clock and not at 4999, the one committed behind it
// a bound later. A reader of either surface goes on there without the holder. When the holder runs again its own bound
// fails the head with Metal's timeout, its Withdraw of what the kernel gave up passes, and the one behind runs.
void stoppedHolderIsReleasedByTheKernelAtItsBound()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue], other = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device), next = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil && next != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);
    id<MTLSharedEvent> event = [o.device newSharedEvent];
    kernel_model::Model &m = kernel_model::model;
    const kernel_model::HooksCleared cleared;
    advanceEventClock(o.device, other, 0);
    dispatch_semaphore_t made = countMakes();
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    std::vector<int> order;
    id<MTLCommandBuffer> held = [queue commandBuffer];
    [held encodeWaitForEvent:event value:1];
    drawInto(held, frame, o.state);
    committed(held, done, &order, 0);
    ASSERT_TRUE(within(made, 10)) << "never held";
    id<MTLCommandBuffer> behind = [queue commandBuffer];
    drawInto(behind, next, o.state);
    committed(behind, done, &order, 1);
    ASSERT_TRUE(within(made, 10)) << "the command buffer behind it was not held";
    advanceEventClock(o.device, other, 0);
    const uint32_t head = m.promiseCalls[0].promise;
    uint32_t later = head;
    for (const auto &call : m.promiseCalls)
        if (call.op == TegraGM20BPromiseMake && call.promise != head)
            later = call.promise;
    ASSERT_NE(later, head);
    EXPECT_EQ(armedMs(head), 5000) << "the holder handed the kernel no bound";
    EXPECT_EQ(armedMs(later), 10000);
    advanceKernelClock(4999);
    EXPECT_FALSE(released(head)) << "given up before the bound";
    advanceKernelClock(1);
    EXPECT_TRUE(released(head)) << "a reader of the stopped holder's surface still waits";
    EXPECT_TRUE(m.promiseGivenUp[head]);
    EXPECT_FALSE(released(later));
    advanceKernelClock(5000);
    EXPECT_TRUE(released(later)) << "a reader of the surface behind it still waits";
    advanceEventClock(o.device, other, 5000);
    for (int i = 0; i < 2; i++)
        ASSERT_TRUE(within(done, 10)) << "command buffer " << i << " never completed";
    EXPECT_EQ(held.status, MTLCommandBufferStatusError);
    EXPECT_EQ(held.error.code, MTLCommandBufferErrorTimeout);
    EXPECT_EQ(behind.status, MTLCommandBufferStatusCompleted) << text(behind.error);
    EXPECT_EQ(order, (std::vector<int>{0, 1}));
}

// The kernel took the head's promise past its bound before the holder ran; the signal that comes after does not run the
// command buffer: its keep is refused (kIOReturnTimeout) and it fails with Metal's timeout, as Metal fails a command
// buffer whose bound passed, never half-run
void signalAfterTheKernelsBoundStillFails()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue], other = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);
    id<MTLSharedEvent> event = [o.device newSharedEvent];
    kernel_model::Model &m = kernel_model::model;
    const kernel_model::HooksCleared cleared;
    advanceEventClock(o.device, other, 0);
    dispatch_semaphore_t made = countMakes();
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    std::vector<int> order;
    id<MTLCommandBuffer> held = [queue commandBuffer];
    [held encodeWaitForEvent:event value:1];
    drawInto(held, frame, o.state);
    committed(held, done, &order, 0);
    ASSERT_TRUE(within(made, 10)) << "never held";
    advanceEventClock(o.device, other, 0);
    const uint32_t promise = m.promiseCalls[0].promise;
    advanceKernelClock(5000);
    ASSERT_TRUE(released(promise));
    event.signaledValue = 1;
    ASSERT_TRUE(within(done, 10)) << "never completed";
    EXPECT_EQ(held.status, MTLCommandBufferStatusError);
    EXPECT_EQ(held.error.code, MTLCommandBufferErrorTimeout);
    EXPECT_EQ(text(held.error.localizedDescription),
              "Caused GPU Timeout Error (00000002:kIOGPUCommandBufferCallbackErrorTimeout)");
    EXPECT_EQ([counters(o.device)[@"failed.event.timeout"] longLongValue], 1);
}

// Metal's waitUntilScheduled of a held command buffer returns at the signal, or at the failure 5 s on (an M4 Max), never
// at the hold
void heldWaitUntilScheduledReturnsAtTheSignalOrTheFailure()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue], other = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);
    const kernel_model::HooksCleared cleared;
    advanceEventClock(o.device, other, 0);
    dispatch_semaphore_t made = countMakes();
    for (const bool signalled : {false, true}) {
        SCOPED_TRACE(signalled);
        id<MTLSharedEvent> event = [o.device newSharedEvent];
        id<MTLCommandBuffer> held = [queue commandBuffer];
        [held encodeWaitForEvent:event value:1];
        drawInto(held, frame, o.state);
        [held commit];
        ASSERT_TRUE(within(made, 10)) << "never held";
        advanceEventClock(o.device, other, 0);
        auto scheduled = std::make_shared<std::promise<void>>();
        std::future<void> returned = scheduled->get_future();
        std::thread waiter([held, scheduled] {
            [held waitUntilScheduled];
            scheduled->set_value();
        });
        EXPECT_NE(returned.wait_for(std::chrono::milliseconds(200)), std::future_status::ready)
            << "returned at the hold";
        if (signalled)
            event.signaledValue = 1;
        else
            advanceEventClock(o.device, other, 5000);
        EXPECT_EQ(returned.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        waiter.join();
        [held waitUntilCompleted];
        EXPECT_EQ(held.status, signalled ? MTLCommandBufferStatusCompleted : MTLCommandBufferStatusError)
            << text(held.error);
    }
}

}  // namespace

TEST(GM20BFacadeHeld, AHeldCommandBufferFailsAtMetalsBound)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        heldCommandBufferFailsAtMetalsBound(false);
    });
}

TEST(GM20BFacadeHeld, AHeldCommandBufferWithWorkSubmittedBeforeItsWaitFailsTheSame)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        heldCommandBufferFailsAtMetalsBound(true);
    });
}

TEST(GM20BFacadeHeld, AHeldCommandBufferSignalledInTimeCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(heldCommandBufferSignalledInTimeCompletes);
}

TEST(GM20BFacadeHeld, AStoppedHoldersReaderGoesOnAtTheKernelsBound)
{
    switch_ios::test::ExpectPassesInFreshProcess(stoppedHolderIsReleasedByTheKernelAtItsBound);
}

TEST(GM20BFacadeHeld, ASignalAfterTheKernelsBoundStillFailsTheCommandBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(signalAfterTheKernelsBoundStillFails);
}

TEST(GM20BFacadeHeld, WaitUntilScheduledReturnsAtTheSignalOrTheFailureNotTheHold)
{
    switch_ios::test::ExpectPassesInFreshProcess(heldWaitUntilScheduledReturnsAtTheSignalOrTheFailure);
}
