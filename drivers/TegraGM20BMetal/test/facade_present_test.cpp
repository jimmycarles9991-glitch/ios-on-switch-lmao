// SPDX-License-Identifier: MIT
// A command buffer's drawables and IOSurface writes on the facade over the kernel model. One that failed before it
// was submitted presents none of its drawables, as Metal's presentDrawable handler presents only from a command buffer
// with no error, and withdraws the writes it was to make (TegraGM20BJobABI.h method 34 Withdraw): its writer promises
// where it was held on a shared event, else the writes themselves, so a present of one of those surfaces from elsewhere
// is refused instead of showing the frame before. One that was submitted presents once and calls method 34 for nothing.
// In the render server such a command buffer ends the process with its cause (TegraGM20BMetalFinish.mm withholdPresents).
#include "facade_drawn.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// libsyscall's abort_with_reason, stood in for as the kernel model stands in for IOKit: its reason on stderr, and the
// process ends as the real one ends it
extern "C" void abort_with_reason(uint32_t, uint64_t, const char *reason, uint64_t)
{
    fprintf(stderr, "abort_with_reason: %s\n", reason ? reason : "");
    fflush(stderr);
    _exit(70);
}

namespace {

using namespace drawn;

// The kernel refuses the job of a command buffer that draws into an IOSurface and presents it three ways: none of the
// presents is made, and with no promise to withdraw the surface's write itself is, naming its import. The next command
// buffer presents once, with the jobs of one that presents nothing and no further call of method 34.
void refusedCommandBufferPresentsNothing()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    kernel_model::Model &m = kernel_model::model;
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);
    CountingDrawable *refused = [CountingDrawable new];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, frame, o.state);
    [cb presentDrawable:refused];
    [cb presentDrawable:refused atTime:0];
    [cb presentDrawable:refused afterMinimumDuration:0];
    m.refuse = 1;
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(refused.presents, 0);
    ASSERT_EQ(m.promiseCalls.size(), 1u);
    EXPECT_EQ(m.promiseCalls[0].op, uint32_t(TegraGM20BPromiseWithdraw));
    ASSERT_EQ(m.promiseCalls[0].handles.size(), 1u);
    EXPECT_EQ(m.imports.count(m.promiseCalls[0].handles[0]), 1u) << "the surface's import";
    jobsOfDrawn(queue, frame, o.state, nil);  // what the refusal left to write is written first
    const size_t plain = jobsOfDrawn(queue, frame, o.state, nil);
    CountingDrawable *shown = [CountingDrawable new];
    EXPECT_EQ(jobsOfDrawn(queue, frame, o.state, shown), plain);
    EXPECT_EQ(shown.presents, 1);
    EXPECT_EQ(m.promiseCalls.size(), 1u);
}

// Held on a shared event nothing signalled, a command buffer promises the IOSurface it writes and arms its bound;
// signalled, its job is refused: the promise is withdrawn, not given back as if its write had come, and its drawable is
// not presented
void heldCommandBufferWithdrawsItsPromise()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);
    id<MTLSharedEvent> event = [o.device newSharedEvent];
    ASSERT_TRUE(event != nil);
    kernel_model::Model &m = kernel_model::model;
    CountingDrawable *drawable = [CountingDrawable new];
    dispatch_semaphore_t made = dispatch_semaphore_create(0);
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.promiseCalled = [made, &m] {
            if (m.promiseCalls.back().op == TegraGM20BPromiseMake)
                dispatch_semaphore_signal(made);
        };
    }
    const kernel_model::HooksCleared cleared;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    [cb encodeWaitForEvent:event value:1];
    drawInto(cb, frame, o.state);
    [cb presentDrawable:drawable];
    [cb commit];
    ASSERT_FALSE(dispatch_semaphore_wait(made, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))))
        << "held: its write is promised";
    ASSERT_EQ(m.promiseCalls[0].op, uint32_t(TegraGM20BPromiseMake));
    const uint32_t promise = m.promiseCalls[0].promise;
    m.refuse = 1;
    event.signaledValue = 1;
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(drawable.presents, 0);
    ASSERT_EQ(m.promiseCalls.size(), 3u);
    EXPECT_EQ(m.promiseCalls[1].op, uint32_t(TegraGM20BPromiseArm)) << "its bound handed to the kernel";
    EXPECT_EQ(m.promiseCalls[2].op, uint32_t(TegraGM20BPromiseWithdraw));
    EXPECT_EQ(m.promiseCalls[2].promise, promise);
    EXPECT_TRUE(m.promiseCalls[2].handles.empty());
}

// A command buffer that failed as it was recorded, committed behind one whose job the kernel is still taking: its
// waitUntilScheduled returns only once its write is withdrawn, so a present its caller then makes of the surface is
// refused rather than showing the frame before. Both are recorded before the submit worker is stopped inside the
// model's SubmitJob, as recording may call the model.
void failedWaitReturnsOnceItsWriteIsWithdrawn()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> elsewhere = [o.device newTextureWithDescriptor:td];
    id<MTLCommandBuffer> failed = [queue commandBuffer];
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = frame;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [failed renderCommandEncoderWithDescriptor:p];
    // a pass of no visibility buffer: Metal's validation stops it, so the command buffer fails as it is recorded
    [e setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    id<MTLCommandBuffer> ahead = [queue commandBuffer];
    drawInto(ahead, elsewhere, o.state);
    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t taken = dispatch_semaphore_create(0), gate = dispatch_semaphore_create(0);
    bool stopped = false;
    m.jobAdded = [&m, &stopped, taken, gate] {
        m.fence(m.jobs.back().channel - 1).completed = m.jobs.back().seqno;
        if (!std::exchange(stopped, true)) {
            dispatch_semaphore_signal(taken);
            dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER);
        }
    };
    [ahead commit];
    dispatch_semaphore_wait(taken, DISPATCH_TIME_FOREVER);
    [failed commit];
    std::promise<size_t> seen;
    std::future<size_t> returned = seen.get_future();
    std::thread waiter([&seen, &m, failed] {
        [failed waitUntilScheduled];
        seen.set_value(m.promiseCalls.size());
    });
    EXPECT_NE(returned.wait_for(std::chrono::milliseconds(200)), std::future_status::ready)
        << "returned while the worker had not reached it";
    dispatch_semaphore_signal(gate);
    EXPECT_EQ(returned.get(), 1u);
    waiter.join();
    [failed waitUntilCompleted];
    [ahead waitUntilCompleted];
    m.jobAdded = nullptr;
    EXPECT_EQ(failed.status, MTLCommandBufferStatusError);
    ASSERT_EQ(m.promiseCalls.size(), 1u);
    EXPECT_EQ(m.promiseCalls[0].op, uint32_t(TegraGM20BPromiseWithdraw));
    EXPECT_EQ(m.promiseCalls[0].handles.size(), 1u);
}

// A presenting command buffer that fails as it is recorded (a pass with no visibility buffer, which Metal's validation
// stops), committed: `drawable` is never presented
id<MTLCommandBuffer> commitFailedFrame(const Opened &o, CountingDrawable *drawable)
{
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    if (!frame) {
        ADD_FAILURE() << "no IOSurface render target";
        return nil;
    }
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = frame;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
    return cb;
}

// The render server's failed frame ends the process, the cause in its reason; the stand-in exits 70 with it. The end
// comes before the command buffer completes (TegraGM20BMetalFinish.mm: withholdPresents, then the status).
void renderServerFailedFrameEndsTheProcess()
{
    const Opened o = openPacked(true);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandBuffer> cb = commitFailedFrame(o, [CountingDrawable new]);
    ASSERT_TRUE(cb != nil);
    [cb waitUntilCompleted];
    ADD_FAILURE() << "the render server's failed frame completed and did not end the process";
}

// Any other process's failed frame presents nothing and carries Metal's error; the process goes on
void clientFailedFrameCarriesMetalsError()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    CountingDrawable *drawable = [CountingDrawable new];
    id<MTLCommandBuffer> cb = commitFailedFrame(o, drawable);
    ASSERT_TRUE(cb != nil);
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(drawable.presents, 0);
}

}  // namespace

TEST(GM20BFacadePresent, TheRenderServersFailedFrameEndsItsProcessWithTheCause)
{
    switch_ios::test::ExpectExitInFreshProcess(
        renderServerFailedFrameEndsTheProcess, 70,
        "abort_with_reason: SwitchMetal: the compositor's frame failed in the driver: .+");
}

TEST(GM20BFacadePresent, AnotherProcesssFailedFrameEndsNoProcess)
{
    switch_ios::test::ExpectPassesInFreshProcess(clientFailedFrameCarriesMetalsError);
}

TEST(GM20BFacadePresent, ACommandBufferTheKernelRefusedPresentsNothingAndWithdrawsItsWrite)
{
    switch_ios::test::ExpectPassesInFreshProcess(refusedCommandBufferPresentsNothing);
}

TEST(GM20BFacadePresent, AHeldCommandBufferTheKernelRefusedWithdrawsItsPromise)
{
    switch_ios::test::ExpectPassesInFreshProcess(heldCommandBufferWithdrawsItsPromise);
}

TEST(GM20BFacadePresent, AFailedCommandBuffersWaitUntilScheduledReturnsOnceItsWriteIsWithdrawn)
{
    switch_ios::test::ExpectPassesInFreshProcess(failedWaitReturnsOnceItsWriteIsWithdrawn);
}
