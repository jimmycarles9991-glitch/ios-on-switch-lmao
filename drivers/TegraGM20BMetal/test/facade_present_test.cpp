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
#include <pthread.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// libsyscall's abort_with_reason, stood in for as the kernel model stands in for IOKit: its reason on stderr, and the
// process ends as the real one ends it
extern "C" void abort_with_reason(uint32_t, uint64_t, const char *reason, uint64_t)
{
    fprintf(stderr, "abort_with_reason: %s\n", reason ? reason : "");
    fflush(stderr);
    _exit(70);
}

struct RecordedPresent {
    enum class Kind : uint8_t {
        Immediate,
        AtTime,
        AfterMinimumDuration
    };

    Kind kind;
    CFTimeInterval value;
    std::string threadName;
};

@interface RecordingDrawable : NSObject <MTLDrawable>
@property (nonatomic, readonly) int presents;
- (void)setOnPresent:(std::function<void(const RecordedPresent &)>)callback;
- (std::vector<RecordedPresent>)recorded;
@end

@implementation RecordingDrawable {
    std::mutex _lock;
    std::vector<RecordedPresent> _events;
    std::function<void(const RecordedPresent &)> _onPresent;
}

static std::string currentThreadName()
{
    char name[64] = "";
    pthread_getname_np(pthread_self(), name, sizeof name);
    return name;
}

- (void)setOnPresent:(std::function<void(const RecordedPresent &)>)callback
{
    std::lock_guard<std::mutex> l(_lock);
    _onPresent = std::move(callback);
}

- (int)presents
{
    std::lock_guard<std::mutex> l(_lock);
    return int(_events.size());
}

- (std::vector<RecordedPresent>)recorded
{
    std::lock_guard<std::mutex> l(_lock);
    return _events;
}

- (void)recordKind:(RecordedPresent::Kind)kind value:(CFTimeInterval)value
{
    const RecordedPresent event{kind, value, currentThreadName()};
    std::function<void(const RecordedPresent &)> cb;
    {
        std::lock_guard<std::mutex> l(_lock);
        _events.push_back(event);
        cb = _onPresent;
    }
    if (cb)
        cb(event);
}

- (void)present
{
    [self recordKind:RecordedPresent::Kind::Immediate value:0];
}

- (void)presentAtTime:(CFTimeInterval)presentationTime
{
    [self recordKind:RecordedPresent::Kind::AtTime value:presentationTime];
}

- (void)presentAfterMinimumDuration:(CFTimeInterval)duration
{
    [self recordKind:RecordedPresent::Kind::AfterMinimumDuration value:duration];
}

- (void)addPresentedHandler:(MTLDrawablePresentedHandler)block
{
    (void)block;
}

- (CFTimeInterval)presentedTime
{
    return 0;
}

- (NSUInteger)drawableID
{
    return 0;
}

@end

namespace {

using namespace drawn;

struct OrderLog {
    std::mutex lock;
    std::vector<std::string> items;

    void add(std::string item)
    {
        std::lock_guard<std::mutex> l(lock);
        items.push_back(std::move(item));
    }

    std::vector<std::string> snapshot()
    {
        std::lock_guard<std::mutex> l(lock);
        return items;
    }
};

struct FenceGate {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<uint32_t, uint32_t>> submitted;
    bool waiting = false;
    bool release = false;
    bool killed = false;

    void install(kernel_model::Model &m, std::function<void(size_t count)> onJob = {})
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.jobAdded = [this, &m, onJob = std::move(onJob)] {
            const uint32_t ch = m.jobs.back().channel;
            const uint32_t seq = m.jobs.back().seqno;
            size_t count = 0;
            {
                std::lock_guard<std::mutex> g(mu);
                submitted.push_back({ch, seq});
                count = submitted.size();
            }
            cv.notify_all();
            if (onJob)
                onJob(count);
        };
        m.blockUntil = [this](uint32_t, uint32_t) {
            std::unique_lock<std::mutex> g(mu);
            waiting = true;
            cv.notify_all();
            cv.wait(g, [&] {
                return release || killed;
            });
            return !killed;
        };
    }

    bool isWaiting()
    {
        std::lock_guard<std::mutex> g(mu);
        return waiting;
    }

    bool waitKernelWait(int seconds = 10)
    {
        std::unique_lock<std::mutex> g(mu);
        return cv.wait_for(g, std::chrono::seconds(seconds), [&] {
            return waiting;
        });
    }

    void completeAll(kernel_model::Model &m)
    {
        std::vector<std::pair<uint32_t, uint32_t>> jobs;
        {
            std::lock_guard<std::mutex> g(mu);
            jobs = submitted;
        }
        {
            std::lock_guard<std::mutex> l(kernel_model::callLock);
            for (const auto &[ch, seq] : jobs)
                __atomic_store_n(&m.fence(ch - 1).completed, seq, __ATOMIC_RELEASE);
        }
        {
            std::lock_guard<std::mutex> g(mu);
            release = true;
        }
        cv.notify_all();
    }

    bool fault(kernel_model::Model &m, uint32_t error)
    {
        if (!waitKernelWait())
            return false;
        uint32_t ch = 0;
        {
            std::lock_guard<std::mutex> g(mu);
            if (submitted.empty())
                return false;
            ch = submitted.back().first;
        }
        {
            std::lock_guard<std::mutex> l(kernel_model::callLock);
            m.kill(ch, error);
        }
        {
            std::lock_guard<std::mutex> g(mu);
            killed = true;
        }
        cv.notify_all();
        return true;
    }
};

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
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.refuse = 1;
    }
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
    uint32_t promiseOp = 0;
    uint32_t promise = 0;
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        ASSERT_FALSE(m.promiseCalls.empty());
        promiseOp = m.promiseCalls[0].op;
        promise = m.promiseCalls[0].promise;
        m.refuse = 1;
    }
    ASSERT_EQ(promiseOp, uint32_t(TegraGM20BPromiseMake));
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
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.jobAdded = [&m, &stopped, taken, gate] {
            __atomic_store_n(&m.fence(m.jobs.back().channel - 1).completed, m.jobs.back().seqno, __ATOMIC_RELEASE);
            if (!std::exchange(stopped, true)) {
                dispatch_semaphore_signal(taken);
                dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER);
            }
        };
    }
    const kernel_model::HooksCleared cleared;
    [ahead commit];
    dispatch_semaphore_wait(taken, DISPATCH_TIME_FOREVER);
    [failed commit];
    std::promise<size_t> seen;
    std::future<size_t> returned = seen.get_future();
    std::thread waiter([&seen, &m, failed] {
        [failed waitUntilScheduled];
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        seen.set_value(m.promiseCalls.size());
    });
    EXPECT_NE(returned.wait_for(std::chrono::milliseconds(200)), std::future_status::ready)
        << "returned while the worker had not reached it";
    dispatch_semaphore_signal(gate);
    EXPECT_EQ(returned.get(), 1u);
    waiter.join();
    [failed waitUntilCompleted];
    [ahead waitUntilCompleted];
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

void nonRenderServerDefersAllThreePresentsUntilCompletion()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    kernel_model::Model &m = kernel_model::model;
    FenceGate gate;
    const kernel_model::HooksCleared cleared;
    gate.install(m);

    OrderLog order;
    OrderLog *orderPtr = &order;
    RecordingDrawable *drawable = [RecordingDrawable new];
    [drawable setOnPresent:[orderPtr](const RecordedPresent &ev) {
        orderPtr->add("present:" + std::to_string(int(ev.kind)));
    }];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, frame, o.state);
    [cb presentDrawable:drawable];
    [cb presentDrawable:drawable atTime:123.5];
    [cb presentDrawable:drawable afterMinimumDuration:0.032];
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        orderPtr->add("completed");
    }];
    [cb commit];
    [cb waitUntilScheduled];
    ASSERT_TRUE(gate.waitKernelWait()) << "presenting non-compositor range must be observed and wait on fence";
    EXPECT_EQ(drawable.presents, 0) << "non-RenderServer presented before GPU fence retired";

    gate.completeAll(m);
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);

    const std::vector<RecordedPresent> events = [drawable recorded];
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].kind, RecordedPresent::Kind::Immediate);
    EXPECT_DOUBLE_EQ(events[0].value, 0.0);
    EXPECT_EQ(events[1].kind, RecordedPresent::Kind::AtTime);
    EXPECT_DOUBLE_EQ(events[1].value, 123.5);
    EXPECT_EQ(events[2].kind, RecordedPresent::Kind::AfterMinimumDuration);
    EXPECT_DOUBLE_EQ(events[2].value, 0.032);
    for (const RecordedPresent &ev : events) {
        EXPECT_NE(ev.threadName, "com.brunostjohn.switch.gm20b.completion");
        EXPECT_NE(ev.threadName, "com.brunostjohn.switch.gm20b.submit");
    }
    EXPECT_EQ(order.snapshot(), (std::vector<std::string>{"present:0", "present:1", "present:2", "completed"}));
}

void deferredPresentsKeepStrictQueueFifoOrder()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    OrderLog order;
    OrderLog *orderPtr = &order;
    dispatch_semaphore_t inHandler1 = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseHandler1 = dispatch_semaphore_create(0);
    RecordingDrawable *d1 = [RecordingDrawable new];
    RecordingDrawable *d2 = [RecordingDrawable new];
    [d1 setOnPresent:[orderPtr](const RecordedPresent &) {
        orderPtr->add("present1");
    }];
    [d2 setOnPresent:[orderPtr](const RecordedPresent &) {
        orderPtr->add("present2");
    }];

    id<MTLCommandBuffer> cb1 = [queue commandBuffer];
    drawInto(cb1, frame, o.state);
    [cb1 presentDrawable:d1];
    [cb1 addCompletedHandler:^(id<MTLCommandBuffer>) {
        orderPtr->add("completed1");
        dispatch_semaphore_signal(inHandler1);
        dispatch_semaphore_wait(releaseHandler1, DISPATCH_TIME_FOREVER);
    }];

    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    drawInto(cb2, frame, o.state);
    [cb2 presentDrawable:d2];
    [cb2 addCompletedHandler:^(id<MTLCommandBuffer>) {
        orderPtr->add("completed2");
    }];

    [cb1 commit];
    [cb2 commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inHandler1, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (cb2.status != MTLCommandBufferStatusCompleted && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_EQ(cb2.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(d1.presents, 1);
    EXPECT_EQ(d2.presents, 0) << "frame 2 presented ahead of frame 1 on the same queue";

    dispatch_semaphore_signal(releaseHandler1);
    [cb1 waitUntilCompleted];
    [cb2 waitUntilCompleted];
    EXPECT_EQ(order.snapshot(), (std::vector<std::string>{"present1", "completed1", "present2", "completed2"}));
}

void postSubmitGpuFaultWithholdsDeferredPresents()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    kernel_model::Model &m = kernel_model::model;
    FenceGate gate;
    const kernel_model::HooksCleared cleared;
    gate.install(m);

    CountingDrawable *drawable = [CountingDrawable new];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, frame, o.state);
    [cb presentDrawable:drawable];
    [cb presentDrawable:drawable atTime:10.0];
    [cb presentDrawable:drawable afterMinimumDuration:0.016];
    [cb commit];
    [cb waitUntilScheduled];
    EXPECT_EQ(drawable.presents, 0);

    ASSERT_TRUE(gate.fault(m, 1));
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorPageFault);
    EXPECT_EQ(drawable.presents, 0) << "faulted command buffer must not present";
}

void renderServerKeepsScheduledTimePresent()
{
    const Opened o = openPacked(true);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    kernel_model::Model &m = kernel_model::model;
    FenceGate gate;
    const kernel_model::HooksCleared cleared;
    gate.install(m);

    CountingDrawable *drawable = [CountingDrawable new];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, frame, o.state);
    [cb presentDrawable:drawable];
    [cb commit];
    [cb waitUntilScheduled];
    EXPECT_EQ(drawable.presents, 1) << "RenderServer must present at scheduled time while fence is in flight";

    gate.completeAll(m);
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(drawable.presents, 1);
}

void postCommitPresentBeforeAndAfterCompletionAndDroppedUncommitted()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    ASSERT_TRUE(frame != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    __weak CountingDrawable *weakDropped = nil;
    @autoreleasepool {
        CountingDrawable *dropped = [CountingDrawable new];
        weakDropped = dropped;
        id<MTLCommandBuffer> uncommitted = [queue commandBuffer];
        [uncommitted presentDrawable:dropped];
    }
    EXPECT_EQ(weakDropped, nil) << "uncommitted command buffer leaked its deferred drawable";

    RecordingDrawable *emptyDrawable = [RecordingDrawable new];
    id<MTLCommandBuffer> emptyCb = [queue commandBuffer];
    [emptyCb presentDrawable:emptyDrawable];
    [emptyCb commit];
    [emptyCb waitUntilCompleted];
    EXPECT_EQ(emptyCb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(emptyDrawable.presents, 1);

    kernel_model::Model &m = kernel_model::model;
    FenceGate gate;
    dispatch_semaphore_t firstInSubmit = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseFirstSubmit = dispatch_semaphore_create(0);
    dispatch_semaphore_t thirdInSubmit = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseThirdSubmit = dispatch_semaphore_create(0);
    const kernel_model::HooksCleared cleared;

    struct Unblock {
        FenceGate &gate;
        kernel_model::Model &m;
        dispatch_semaphore_t first, third;
        bool active = true;

        ~Unblock()
        {
            if (!active)
                return;
            dispatch_semaphore_signal(first);
            dispatch_semaphore_signal(third);
            gate.completeAll(m);
        }
    };

    Unblock unblock{gate, m, releaseFirstSubmit, releaseThirdSubmit};
    gate.install(m, [&](size_t count) {
        if (count == 1) {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            dispatch_semaphore_signal(firstInSubmit);
            dispatch_semaphore_wait(releaseFirstSubmit, DISPATCH_TIME_FOREVER);
        } else if (count == 3) {
            dispatch_semaphore_signal(thirdInSubmit);
            dispatch_semaphore_wait(releaseThirdSubmit, DISPATCH_TIME_FOREVER);
        }
    });

    id<MTLCommandBuffer> cb0 = [queue commandBuffer];
    drawInto(cb0, frame, o.state);
    id<MTLCommandBuffer> cb1 = [queue commandBuffer];
    drawInto(cb1, frame, o.state);
    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    drawInto(cb2, frame, o.state);
    [cb0 commit];
    ASSERT_FALSE(dispatch_semaphore_wait(firstInSubmit, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb1 commit];
    [cb2 commit];
    dispatch_semaphore_signal(releaseFirstSubmit);
    ASSERT_FALSE(dispatch_semaphore_wait(thirdInSubmit, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb1 waitUntilScheduled];
    EXPECT_FALSE(gate.isWaiting()) << "quiet range must not wake completionLoop before presentDrawable";

    RecordingDrawable *before = [RecordingDrawable new];
    [cb1 presentDrawable:before];
    EXPECT_EQ(before.presents, 0);
    ASSERT_TRUE(gate.waitKernelWait(2)) << "post-commit presentDrawable must wake completionLoop on a quiet range";

    unblock.active = false;
    dispatch_semaphore_signal(releaseThirdSubmit);
    gate.completeAll(m);
    [cb0 waitUntilCompleted];
    [cb1 waitUntilCompleted];
    [cb2 waitUntilCompleted];
    ASSERT_EQ(cb1.status, MTLCommandBufferStatusCompleted);
    const std::vector<RecordedPresent> beforeEvents = [before recorded];
    ASSERT_EQ(beforeEvents.size(), 1u);
    EXPECT_EQ(beforeEvents[0].kind, RecordedPresent::Kind::Immediate);
    EXPECT_NE(beforeEvents[0].threadName, "com.brunostjohn.switch.gm20b.completion");

    dispatch_semaphore_t afterDone = dispatch_semaphore_create(0);
    RecordingDrawable *after = [RecordingDrawable new];
    [after setOnPresent:[afterDone](const RecordedPresent &) {
        dispatch_semaphore_signal(afterDone);
    }];
    [cb1 presentDrawable:after atTime:42.25];
    ASSERT_FALSE(dispatch_semaphore_wait(afterDone, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    const std::vector<RecordedPresent> afterEvents = [after recorded];
    ASSERT_EQ(afterEvents.size(), 1u);
    EXPECT_EQ(afterEvents[0].kind, RecordedPresent::Kind::AtTime);
    EXPECT_DOUBLE_EQ(afterEvents[0].value, 42.25);
    EXPECT_NE(afterEvents[0].threadName, "com.brunostjohn.switch.gm20b.completion");

    kernel_model::clearHooks();
    id<MTLCommandQueue> failQueue = [o.device newCommandQueue];
    dispatch_queue_t serial = dispatch_queue_create("com.brunostjohn.switch.test.present.fail", DISPATCH_QUEUE_SERIAL);
    ((SwitchMetalQueue *)failQueue)->_completionQueue = serial;
    id<MTLCommandBuffer> cbFail = [failQueue commandBuffer];
    drawInto(cbFail, frame, o.state);
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.refuse = 1;
    }
    [cbFail commit];
    [cbFail waitUntilCompleted];
    ASSERT_EQ(cbFail.status, MTLCommandBufferStatusError);
    CountingDrawable *afterFail = [CountingDrawable new];
    [cbFail presentDrawable:afterFail];
    dispatch_sync(serial, ^{
                  });
    EXPECT_EQ(afterFail.presents, 0);
}

id<MTLTexture> plainTarget(id<MTLDevice> device)
{
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    return [device newTextureWithDescriptor:td];
}

void drawManyInto(id<MTLCommandBuffer> cb, id<MTLTexture> t, id<MTLRenderPipelineState> state, int passes)
{
    for (int p = 0; p < passes; ++p)
        drawInto(cb, t, state);
}

struct SemGuard {
    dispatch_semaphore_t sem;
    bool released = false;

    void release()
    {
        if (!released) {
            released = true;
            dispatch_semaphore_signal(sem);
        }
    }

    ~SemGuard()
    {
        release();
    }
};

bool waitScheduledWithin(id<MTLCommandBuffer> cb, std::chrono::milliseconds timeout, std::thread &waiter)
{
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> waited = done->get_future();
    waiter = std::thread([cb, done] {
        [cb waitUntilScheduled];
        done->set_value();
    });
    return waited.wait_for(timeout) == std::future_status::ready;
}

void compositorEarlyScheduledPresentsOnPromiseBeforeLastRangeSubmits()
{
    const Opened o = openPacked(true);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLCommandQueue> blockQueue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    id<MTLTexture> scratch = plainTarget(o.device);
    ASSERT_TRUE(frame != nil && scratch != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    CountingDrawable *drawable = [CountingDrawable new];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawManyInto(cb, frame, o.state, 15);
    [cb presentDrawable:drawable];
    id<MTLCommandBuffer> blocker = [blockQueue commandBuffer];
    drawInto(blocker, scratch, o.state);

    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t inBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t inJob = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseJob = dispatch_semaphore_create(0);
    std::atomic<int> jobNo{0};
    const size_t jobsBefore = m.jobs.size();
    const kernel_model::HooksCleared cleared;
    std::atomic<size_t> makes{0};
    SemGuard blockGuard{releaseBlock};
    SemGuard jobGuard{releaseJob};

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.promiseCalled = [&m, &makes] {
            if (m.promiseCalls.back().op == TegraGM20BPromiseMake)
                makes.fetch_add(1, std::memory_order_release);
        };
        m.jobAdded = [&m, &jobNo, inBlock, releaseBlock, inJob, releaseJob] {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            const int n = jobNo.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 1) {
                dispatch_semaphore_signal(inBlock);
                dispatch_semaphore_wait(releaseBlock, DISPATCH_TIME_FOREVER);
            } else if (n == 2) {
                dispatch_semaphore_signal(inJob);
                dispatch_semaphore_wait(releaseJob, DISPATCH_TIME_FOREVER);
            }
        };
    }

    [blocker commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inBlock, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb commit];
    blockGuard.release();

    ASSERT_FALSE(dispatch_semaphore_wait(inJob, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))))
        << "first range never entered SubmitJob";
    std::thread waiter;
    const bool early = waitScheduledWithin(cb, std::chrono::seconds(2), waiter);
    EXPECT_TRUE(early) << "RenderServer waitUntilScheduled blocked on the submit worker";
    EXPECT_EQ(makes.load(std::memory_order_acquire), 1u);
    if (early) {
        EXPECT_EQ(drawable.presents, 1);
        EXPECT_EQ(cb.status, MTLCommandBufferStatusScheduled);
    }

    jobGuard.release();
    waiter.join();
    [blocker waitUntilCompleted];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(drawable.presents, 1);

    std::lock_guard<std::mutex> l(kernel_model::callLock);
    ASSERT_GT(m.jobs.size(), jobsBefore + 2);
    EXPECT_EQ(m.jobs[jobsBefore + 1].flags & TegraGM20BJobFulfills, 0u);
    EXPECT_NE(m.jobs.back().flags & TegraGM20BJobFulfills, 0u);
    ASSERT_EQ(m.promiseCalls.size(), 1u);
    EXPECT_EQ(m.promiseCalls[0].op, uint32_t(TegraGM20BPromiseMake));
    ASSERT_EQ(m.promiseCalls[0].handles.size(), 1u);
    EXPECT_EQ(m.imports.count(m.promiseCalls[0].handles[0]), 1u);
}

void compositorEarlyScheduledRefusedFrameWithdrawsPromiseAndEndsProcess()
{
    const Opened o = openPacked(true);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLCommandQueue> blockQueue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    id<MTLTexture> scratch = plainTarget(o.device);
    ASSERT_TRUE(frame != nil && scratch != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    CountingDrawable *drawable = [CountingDrawable new];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawManyInto(cb, frame, o.state, 15);
    [cb presentDrawable:drawable];
    id<MTLCommandBuffer> blocker = [blockQueue commandBuffer];
    drawInto(blocker, scratch, o.state);

    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t inBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t inJob = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseJob = dispatch_semaphore_create(0);
    std::atomic<int> jobNo{0};
    const kernel_model::HooksCleared cleared;
    SemGuard blockGuard{releaseBlock};
    SemGuard jobGuard{releaseJob};

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.promiseCalled = [&m] {
            const auto &call = m.promiseCalls.back();
            if (call.op == TegraGM20BPromiseWithdraw) {
                fprintf(stderr, "promise_withdraw:%u\n", call.promise);
                fflush(stderr);
            }
        };
        m.jobAdded = [&m, &jobNo, inBlock, releaseBlock, inJob, releaseJob] {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            const int n = jobNo.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 1) {
                dispatch_semaphore_signal(inBlock);
                dispatch_semaphore_wait(releaseBlock, DISPATCH_TIME_FOREVER);
            } else if (n == 2) {
                dispatch_semaphore_signal(inJob);
                dispatch_semaphore_wait(releaseJob, DISPATCH_TIME_FOREVER);
                m.refuse = 1;
            }
        };
    }

    [blocker commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inBlock, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb commit];
    blockGuard.release();

    ASSERT_FALSE(dispatch_semaphore_wait(inJob, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    std::thread waiter;
    const bool early = waitScheduledWithin(cb, std::chrono::seconds(2), waiter);
    if (!early) {
        jobGuard.release();
        waiter.join();
        ADD_FAILURE() << "RenderServer waitUntilScheduled blocked before refusal";
        return;
    }
    waiter.join();
    ASSERT_EQ(drawable.presents, 1);
    jobGuard.release();
    [cb waitUntilCompleted];
    ADD_FAILURE() << "refused early-scheduled RenderServer frame did not end the process";
}

void compositorEarlyScheduledRefusedNonPresentingFrameWithdrawsPromise()
{
    const Opened o = openPacked(true);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLCommandQueue> blockQueue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    id<MTLTexture> scratch = plainTarget(o.device);
    ASSERT_TRUE(frame != nil && scratch != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawManyInto(cb, frame, o.state, 15);
    id<MTLCommandBuffer> blocker = [blockQueue commandBuffer];
    drawInto(blocker, scratch, o.state);

    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t inBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t inJob = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseJob = dispatch_semaphore_create(0);
    std::atomic<int> jobNo{0};
    const kernel_model::HooksCleared cleared;
    SemGuard blockGuard{releaseBlock};
    SemGuard jobGuard{releaseJob};

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.jobAdded = [&m, &jobNo, inBlock, releaseBlock, inJob, releaseJob] {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            const int n = jobNo.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 1) {
                dispatch_semaphore_signal(inBlock);
                dispatch_semaphore_wait(releaseBlock, DISPATCH_TIME_FOREVER);
            } else if (n == 2) {
                dispatch_semaphore_signal(inJob);
                dispatch_semaphore_wait(releaseJob, DISPATCH_TIME_FOREVER);
                m.refuse = 1;
            }
        };
    }

    [blocker commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inBlock, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb commit];
    blockGuard.release();

    ASSERT_FALSE(dispatch_semaphore_wait(inJob, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    std::thread waiter;
    const bool early = waitScheduledWithin(cb, std::chrono::seconds(2), waiter);
    EXPECT_TRUE(early);
    jobGuard.release();
    waiter.join();
    [blocker waitUntilCompleted];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        ASSERT_EQ(m.promiseCalls.size(), 2u);
        EXPECT_EQ(m.promiseCalls[0].op, uint32_t(TegraGM20BPromiseMake));
        EXPECT_EQ(m.promiseCalls[1].op, uint32_t(TegraGM20BPromiseWithdraw));
        EXPECT_EQ(m.promiseCalls[1].promise, m.promiseCalls[0].promise);
        EXPECT_TRUE(m.promiseCalls[1].handles.empty());
        m.jobAdded = nullptr;
    }

    id<MTLCommandBuffer> next = [queue commandBuffer];
    drawInto(next, frame, o.state);
    [next commit];
    [next waitUntilCompleted];
    EXPECT_EQ(next.status, MTLCommandBufferStatusCompleted);
}

void nonCompositorClientSchedulesWithoutPromisesAndDefersPresent()
{
    const Opened o = openPacked(false);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLCommandQueue> blockQueue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    id<MTLTexture> scratch = plainTarget(o.device);
    ASSERT_TRUE(frame != nil && scratch != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    CountingDrawable *drawable = [CountingDrawable new];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawManyInto(cb, frame, o.state, 15);
    [cb presentDrawable:drawable];
    id<MTLCommandBuffer> blocker = [blockQueue commandBuffer];
    drawInto(blocker, scratch, o.state);

    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t inBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t inJob = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseJob = dispatch_semaphore_create(0);
    std::atomic<int> jobNo{0};
    std::atomic<size_t> makes{0};
    const kernel_model::HooksCleared cleared;
    SemGuard blockGuard{releaseBlock};
    SemGuard jobGuard{releaseJob};

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.promiseCalled = [&m, &makes] {
            if (m.promiseCalls.back().op == TegraGM20BPromiseMake)
                makes.fetch_add(1, std::memory_order_release);
        };
        m.jobAdded = [&m, &jobNo, inBlock, releaseBlock, inJob, releaseJob] {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            const int n = jobNo.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 1) {
                dispatch_semaphore_signal(inBlock);
                dispatch_semaphore_wait(releaseBlock, DISPATCH_TIME_FOREVER);
            } else if (n == 2) {
                dispatch_semaphore_signal(inJob);
                dispatch_semaphore_wait(releaseJob, DISPATCH_TIME_FOREVER);
            }
        };
    }

    [blocker commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inBlock, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb commit];
    blockGuard.release();

    ASSERT_FALSE(dispatch_semaphore_wait(inJob, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    std::thread waiter;
    const bool early = waitScheduledWithin(cb, std::chrono::milliseconds(200), waiter);
    EXPECT_TRUE(early) << "an app's waitUntilScheduled waited for its job's submission";
    EXPECT_EQ(drawable.presents, 0) << "an app's drawable was presented before its command buffer completed";
    EXPECT_EQ(makes.load(std::memory_order_acquire), 0u) << "an app made a writer promise";

    jobGuard.release();
    waiter.join();
    [blocker waitUntilCompleted];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(drawable.presents, 1);
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        EXPECT_TRUE(m.promiseCalls.empty());
    }
}

void nonCompositorSurfaceWriterWaitsForSubmissionBeforeScheduled()
{
    const Opened o = openPacked(false);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLCommandQueue> blockQueue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    id<MTLTexture> scratch = plainTarget(o.device);
    ASSERT_TRUE(frame != nil && scratch != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, frame, o.state);
    id<MTLCommandBuffer> blocker = [blockQueue commandBuffer];
    drawInto(blocker, scratch, o.state);

    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t inBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseBlock = dispatch_semaphore_create(0);
    std::atomic<int> jobNo{0};
    const kernel_model::HooksCleared cleared;
    SemGuard blockGuard{releaseBlock};

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.jobAdded = [&m, &jobNo, inBlock, releaseBlock] {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            if (jobNo.fetch_add(1, std::memory_order_acq_rel) == 0) {
                dispatch_semaphore_signal(inBlock);
                dispatch_semaphore_wait(releaseBlock, DISPATCH_TIME_FOREVER);
            }
        };
    }

    [blocker commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inBlock, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb commit];

    std::thread waiter;
    const bool early = waitScheduledWithin(cb, std::chrono::milliseconds(150), waiter);
    EXPECT_FALSE(early) << "non-presenting IOSurface writer scheduled before kernel submission";

    blockGuard.release();
    waiter.join();
    [blocker waitUntilCompleted];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
}

void compositorSerializesEarlyPromisesOnSameSurface()
{
    const Opened o = openPacked(true);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLCommandQueue> blockQueue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    id<MTLTexture> scratch = plainTarget(o.device);
    ASSERT_TRUE(frame != nil && scratch != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    id<MTLCommandBuffer> cb1 = [queue commandBuffer];
    drawManyInto(cb1, frame, o.state, 15);
    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    drawManyInto(cb2, frame, o.state, 15);
    id<MTLCommandBuffer> blocker = [blockQueue commandBuffer];
    drawInto(blocker, scratch, o.state);

    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t inBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t inRange0 = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseRange0 = dispatch_semaphore_create(0);
    dispatch_semaphore_t inRange1 = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseRange1 = dispatch_semaphore_create(0);
    std::atomic<int> jobNo{0};
    std::atomic<size_t> makes{0};
    const kernel_model::HooksCleared cleared;
    SemGuard blockGuard{releaseBlock};
    SemGuard range0Guard{releaseRange0};
    SemGuard range1Guard{releaseRange1};

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.promiseCalled = [&m, &makes] {
            if (m.promiseCalls.back().op == TegraGM20BPromiseMake)
                makes.fetch_add(1, std::memory_order_release);
        };
        m.jobAdded = [&m, &jobNo, inBlock, releaseBlock, inRange0, releaseRange0, inRange1, releaseRange1] {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            const int n = jobNo.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 1) {
                dispatch_semaphore_signal(inBlock);
                dispatch_semaphore_wait(releaseBlock, DISPATCH_TIME_FOREVER);
            } else if (n == 2) {
                dispatch_semaphore_signal(inRange0);
                dispatch_semaphore_wait(releaseRange0, DISPATCH_TIME_FOREVER);
            } else if (n == 3) {
                dispatch_semaphore_signal(inRange1);
                dispatch_semaphore_wait(releaseRange1, DISPATCH_TIME_FOREVER);
            }
        };
    }

    [blocker commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inBlock, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb1 commit];
    blockGuard.release();

    ASSERT_FALSE(dispatch_semaphore_wait(inRange0, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    std::thread waiter1;
    const bool early1 = waitScheduledWithin(cb1, std::chrono::seconds(2), waiter1);
    EXPECT_TRUE(early1) << "first compositor CB must early-schedule";
    EXPECT_EQ(makes.load(std::memory_order_acquire), 1u);

    [cb2 commit];
    range0Guard.release();
    waiter1.join();

    // Waiting for cb1's second range guarantees the submit worker ran applyChange(cb2) while cb1 still held its
    // early-promised surface entry.
    ASSERT_FALSE(dispatch_semaphore_wait(inRange1, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    std::thread waiter2;
    const bool early2 = waitScheduledWithin(cb2, std::chrono::milliseconds(200), waiter2);
    EXPECT_FALSE(early2) << "second compositor CB writing same IOSurface must wait until first early promise retires";
    EXPECT_EQ(makes.load(std::memory_order_acquire), 1u);

    range1Guard.release();
    waiter2.join();
    [blocker waitUntilCompleted];
    [cb1 waitUntilCompleted];
    [cb2 waitUntilCompleted];
    EXPECT_EQ(cb1.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(cb2.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(makes.load(std::memory_order_acquire), 2u);
}

void compositorEarlyPromisesCoverSurfaceBufferAndTextureBufferWrites()
{
    const Opened o = openPacked(true);
    ASSERT_TRUE(o.device != nil && o.state != nil) << o.why;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    id<MTLCommandQueue> blockQueue = [o.device newCommandQueue];
    id<MTLTexture> frame = surfaceTarget(o.device);
    id<MTLTexture> scratch = plainTarget(o.device);
    ASSERT_TRUE(frame != nil && scratch != nil);
    ASSERT_GE(jobsOfDrawn(queue, frame, o.state, nil), 1u);

    IOSurfaceRef bufSurface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @256,
        (id)kIOSurfaceAllocSize : @(64 * 256),
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_NE(bufSurface, nullptr);
    id<MTLBuffer> surfBuf = [(id)o.device newBufferWithIOSurface:bufSurface];
    CFRelease(bufSurface);
    ASSERT_NE(surfBuf, nil);

    IOSurfaceRef tbSurface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @256,
        (id)kIOSurfaceAllocSize : @(64 * 256),
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_NE(tbSurface, nullptr);
    id<MTLBuffer> tbBuf = [(id)o.device newBufferWithIOSurface:tbSurface];
    CFRelease(tbSurface);
    ASSERT_NE(tbBuf, nil);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texBuf = [tbBuf newTextureWithDescriptor:td offset:0 bytesPerRow:256];
    ASSERT_NE(texBuf, nil);

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawManyInto(cb, frame, o.state, 10);
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:surfBuf range:NSMakeRange(0, 256) value:0x5a];
    [blit endEncoding];
    drawInto(cb, texBuf, o.state);
    id<MTLCommandBuffer> blocker = [blockQueue commandBuffer];
    drawInto(blocker, scratch, o.state);

    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t inBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseBlock = dispatch_semaphore_create(0);
    dispatch_semaphore_t inJob = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseJob = dispatch_semaphore_create(0);
    std::atomic<int> jobNo{0};
    const kernel_model::HooksCleared cleared;
    SemGuard blockGuard{releaseBlock};
    SemGuard jobGuard{releaseJob};

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.jobAdded = [&m, &jobNo, inBlock, releaseBlock, inJob, releaseJob] {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            const int n = jobNo.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 1) {
                dispatch_semaphore_signal(inBlock);
                dispatch_semaphore_wait(releaseBlock, DISPATCH_TIME_FOREVER);
            } else if (n == 2) {
                dispatch_semaphore_signal(inJob);
                dispatch_semaphore_wait(releaseJob, DISPATCH_TIME_FOREVER);
            }
        };
    }

    [blocker commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inBlock, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    [cb commit];
    blockGuard.release();

    ASSERT_FALSE(dispatch_semaphore_wait(inJob, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))));
    std::thread waiter;
    const bool early = waitScheduledWithin(cb, std::chrono::seconds(2), waiter);
    EXPECT_TRUE(early) << "compositor CB writing surface buffer and texture-buffer must early-schedule";
    jobGuard.release();
    waiter.join();
    [blocker waitUntilCompleted];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);

    std::lock_guard<std::mutex> l(kernel_model::callLock);
    ASSERT_EQ(m.promiseCalls.size(), 1u);
    EXPECT_EQ(m.promiseCalls[0].op, uint32_t(TegraGM20BPromiseMake));
    EXPECT_EQ(m.promiseCalls[0].handles.size(), 3u);
    for (uint64_t h : m.promiseCalls[0].handles)
        EXPECT_EQ(m.imports.count(h), 1u);
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
#if __has_feature(thread_sanitizer)
    // IOSurfaceSharedEvent delivers its Mach notification callback through uninstrumented IOSurface.framework.
    GTEST_SKIP();
#endif
    switch_ios::test::ExpectPassesInFreshProcess(heldCommandBufferWithdrawsItsPromise);
}

TEST(GM20BFacadePresent, AFailedCommandBuffersWaitUntilScheduledReturnsOnceItsWriteIsWithdrawn)
{
    switch_ios::test::ExpectPassesInFreshProcess(failedWaitReturnsOnceItsWriteIsWithdrawn);
}

TEST(GM20BFacadePresent, NonRenderServerDefersAllThreePresentFormsUntilGpuCompletion)
{
    switch_ios::test::ExpectPassesInFreshProcess(nonRenderServerDefersAllThreePresentsUntilCompletion);
}

TEST(GM20BFacadePresent, DeferredPresentsKeepStrictPerQueueFifoOrder)
{
    switch_ios::test::ExpectPassesInFreshProcess(deferredPresentsKeepStrictQueueFifoOrder);
}

TEST(GM20BFacadePresent, PostSubmitGpuFaultWithholdsDeferredPresents)
{
    switch_ios::test::ExpectPassesInFreshProcess(postSubmitGpuFaultWithholdsDeferredPresents);
}

TEST(GM20BFacadePresent, RenderServerKeepsScheduledTimePresent)
{
    switch_ios::test::ExpectPassesInFreshProcess(renderServerKeepsScheduledTimePresent);
}

TEST(GM20BFacadePresent, PostCommitPresentsAndUncommittedDropBehaveCleanly)
{
    switch_ios::test::ExpectPassesInFreshProcess(postCommitPresentBeforeAndAfterCompletionAndDroppedUncommitted);
}

TEST(GM20BFacadePresent, CompositorEarlyScheduledPresentsOnPromiseBeforeLastRangeSubmits)
{
    switch_ios::test::ExpectPassesInFreshProcess(compositorEarlyScheduledPresentsOnPromiseBeforeLastRangeSubmits);
}

TEST(GM20BFacadePresent, CompositorEarlyScheduledRefusedFrameWithdrawsPromiseAndEndsProcess)
{
    switch_ios::test::ExpectExitInFreshProcess(
        compositorEarlyScheduledRefusedFrameWithdrawsPromiseAndEndsProcess, 70,
        "promise_withdraw:[0-9]+\nabort_with_reason: SwitchMetal: the compositor's frame failed in the driver: .+");
}

TEST(GM20BFacadePresent, CompositorEarlyScheduledRefusedNonPresentingFrameWithdrawsPromise)
{
    switch_ios::test::ExpectPassesInFreshProcess(compositorEarlyScheduledRefusedNonPresentingFrameWithdrawsPromise);
}

TEST(GM20BFacadePresent, NonCompositorClientSchedulesWithoutPromisesAndDefersPresent)
{
    switch_ios::test::ExpectPassesInFreshProcess(nonCompositorClientSchedulesWithoutPromisesAndDefersPresent);
}

TEST(GM20BFacadePresent, NonCompositorSurfaceWriterWaitsForSubmissionBeforeScheduled)
{
    switch_ios::test::ExpectPassesInFreshProcess(nonCompositorSurfaceWriterWaitsForSubmissionBeforeScheduled);
}

TEST(GM20BFacadePresent, CompositorSerializesEarlyPromisesOnSameSurface)
{
    switch_ios::test::ExpectPassesInFreshProcess(compositorSerializesEarlyPromisesOnSameSurface);
}

TEST(GM20BFacadePresent, CompositorEarlyPromisesCoverSurfaceBufferAndTextureBufferWrites)
{
    switch_ios::test::ExpectPassesInFreshProcess(compositorEarlyPromisesCoverSurfaceBufferAndTextureBufferWrites);
}
