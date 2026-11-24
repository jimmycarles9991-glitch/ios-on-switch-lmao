// SPDX-License-Identifier: MIT
// Core Animation shaped command buffers through the facade over the kernel model, recorded at the render thread's
// pace (a pass every few hundred microseconds), so the submit worker encodes the ended encoders ahead of the commit
// (TegraGM20BMetalPipeline.mm): what was encoded ahead and what took it back; and, with a GPU that holds each job, the time from
// the commit to its return, to waitUntilScheduled's and to the first job's submission (commit_latency.txt).
#include "scale_frames.h"
#include "timed_gpu.h"
#include "bazel/testing/fresh_process.h"
#include "../TegraGM20BMetalPassCache.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <thread>

// A buffer this device did not make
@interface ForeignBuffer : NSObject
@end

@implementation ForeignBuffer

- (NSUInteger)length
{
    return 64;
}

@end

@interface EmptyFastPathDrawable : NSObject <MTLDrawable>
@property (nonatomic) std::atomic<int> *presents;
@end

@implementation EmptyFastPathDrawable

- (void)present
{
    if (_presents)
        _presents->fetch_add(1, std::memory_order_acq_rel);
}

- (void)presentAtTime:(CFTimeInterval)t
{
    (void)t;
    [self present];
}

- (void)presentAfterMinimumDuration:(CFTimeInterval)d
{
    (void)d;
    [self present];
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

using namespace harness;
using Clock = std::chrono::steady_clock;

using scale_frames::opened;

double percentileOf(std::vector<double> v, double p)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
}

std::string outputs()
{
    const char *at = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    return at ? at : dir();
}

size_t jobCount()
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    return kernel_model::model.jobs.size();
}

// `shape` recorded on `queue` a pass every `pace`, each pass beside `beside` where there is one
id<MTLCommandBuffer> paced(id<MTLCommandQueue> queue, const std::vector<id<MTLTexture>> &into,
                           const scale_frames::Shape &shape, uint32_t frame, std::chrono::microseconds pace,
                           id<MTLTexture> beside = nil)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        scale_frames::pass(cb, opened().state, into, shape, p, frame, beside);
        std::this_thread::sleep_for(pace);
    }
    return cb;
}

// What the worker encoded of `cb` ahead of its commit (SwitchMetalCommandBuffer switchMetalPipelined); empty on a build without it
NSDictionary *pipelined(id<MTLCommandBuffer> cb)
{
    const SEL selector = sel_registerName("switchMetalPipelined");
    if (![(id)cb respondsToSelector:selector])
        return nil;
    return ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)cb, selector);
}

std::string path(id<MTLCommandBuffer> cb)
{
    return [[(id)cb performSelector:@selector(switchMetalPath)] UTF8String];
}

void compositorWaitUntilScheduledReturnsBeforeLongRangeSubmits()
{
    const std::string pack = harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm));
    const std::string metallib = harness::writeMetallib();
    metal_prepare(pack.c_str(), 0);
    id<MTLDevice> device = harness::open(pack);
    ASSERT_NE(device, nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_NE(library, nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:harness::descriptor(library, MTLPixelFormatBGRA8Unorm)
                                               error:&error];
    ASSERT_NE(state, nil);

    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @256,
        (id)kIOSurfaceAllocSize : @(64 * 256),
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_NE(surface, nullptr);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> frame = [device newTextureWithDescriptor:td iosurface:surface plane:0];
    CFRelease(surface);
    ASSERT_NE(frame, nil);

    id<MTLCommandQueue> queue = [device newCommandQueue];
    const scale_frames::Shape warmShape{1, 4, 1, 64, 64};
    id<MTLCommandBuffer> warm = scale_frames::record(queue, state, {frame}, warmShape, 0);
    [warm commit];
    [warm waitUntilCompleted];
    ASSERT_EQ(warm.status, MTLCommandBufferStatusCompleted);

    kernel_model::Model &m = kernel_model::model;
    dispatch_semaphore_t inJob = dispatch_semaphore_create(0);
    dispatch_semaphore_t releaseJob = dispatch_semaphore_create(0);
    std::atomic<bool> held{false};
    const size_t jobsBefore = jobCount();
    const kernel_model::HooksCleared cleared;
    std::atomic<size_t> makeCalls{0};

    struct ReleaseGuard {
        dispatch_semaphore_t sem;
        bool released = false;

        ~ReleaseGuard()
        {
            if (!released)
                dispatch_semaphore_signal(sem);
        }
    } guard{releaseJob};

    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        m.promiseCalled = [&m, &makeCalls] {
            if (m.promiseCalls.back().op == TegraGM20BPromiseMake)
                makeCalls.fetch_add(1, std::memory_order_release);
        };
        m.jobAdded = [&m, &held, inJob, releaseJob] {
            const auto &j = m.jobs.back();
            __atomic_store_n(&m.fence(j.channel - 1).completed, j.seqno, __ATOMIC_RELEASE);
            if (!held.exchange(true, std::memory_order_acq_rel)) {
                dispatch_semaphore_signal(inJob);
                dispatch_semaphore_wait(releaseJob, DISPATCH_TIME_FOREVER);
            }
        };
    }

    const scale_frames::Shape shape{15, 90, 1, 64, 64};
    id<MTLCommandBuffer> cb = scale_frames::record(queue, state, {frame}, shape, 1);
    [cb commit];
    ASSERT_FALSE(dispatch_semaphore_wait(inJob, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))))
        << "first range never entered SubmitJob";

    auto done = std::make_shared<std::promise<void>>();
    std::future<void> waited = done->get_future();
    std::thread waiter([cb, done] {
        [cb waitUntilScheduled];
        done->set_value();
    });
    const bool early = waited.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(early) << "waitUntilScheduled blocked until the submit worker finished the long range";
    EXPECT_GE(makeCalls.load(std::memory_order_acquire), 1u);
    if (early)
        EXPECT_EQ(cb.status, MTLCommandBufferStatusScheduled);

    guard.released = true;
    dispatch_semaphore_signal(releaseJob);
    waiter.join();
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);

    std::lock_guard<std::mutex> l(kernel_model::callLock);
    ASSERT_GE(m.jobs.size(), jobsBefore + 1);
    for (size_t i = jobsBefore; i + 1 < m.jobs.size(); ++i)
        EXPECT_EQ(m.jobs[i].flags & TegraGM20BJobFulfills, 0u);
    EXPECT_NE(m.jobs.back().flags & TegraGM20BJobFulfills, 0u);
    EXPECT_EQ(m.promiseCalls.size(), 1u);
}

TEST(GM20BFacadeCommit, CompositorWaitUntilScheduledReturnsBeforeLongRangeSubmits)
{
    switch_ios::test::ExpectPassesInFreshProcess(compositorWaitUntilScheduledReturnsBeforeLongRangeSubmits);
}

TEST(GM20BFacadeCommit, ParallelRecordingsSurviveTrimAndReturnTheirBanksAfterSplice)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const auto into = scale_frames::targets(opened().device, {1, 4, 1});
    for (int frame = 0; frame < 4; frame++) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = into[0];
        d.colorAttachments[0].loadAction = MTLLoadActionClear;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLParallelRenderCommandEncoder> parallel = [cb parallelRenderCommandEncoderWithDescriptor:d];
        ASSERT_NE(parallel, nil);
        std::vector<std::thread> workers;
        for (int sub = 0; sub < 4; sub++) {
            id<MTLRenderCommandEncoder> e = [parallel renderCommandEncoder];
            EXPECT_NE(e, nil);
            workers.emplace_back([e] {
                @autoreleasepool {
                    const float vertices[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
                    const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                    const float colour[4] = {1, 0, 0, 1};
                    [e setRenderPipelineState:opened().state];
                    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
                    [e setVertexBytes:transform length:sizeof transform atIndex:2];
                    [e setFragmentBytes:colour length:sizeof colour atIndex:0];
                    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                    [e endEncoding];
                }
            });
        }
        gm20bPassTrim();
        for (std::thread &worker : workers)
            worker.join();
        [parallel endEncoding];
        gm20bPassTrim();
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
        EXPECT_EQ(path(cb), "gm20b");
    }
}

TEST(GM20BFacadeCommit, EncodersEndedBeforeTheCommitAreEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    NSDictionary *before = counters(opened().device);
    for (uint32_t frame = 0; frame < 4; frame++) {
        // the first frame with the worker held through its recording, as on a loaded host
        std::optional<scale_frames::WorkerHold> hold;
        if (!frame)
            hold.emplace();
        id<MTLCommandBuffer> cb = paced(queue, into, shape, frame, std::chrono::milliseconds(1));
        hold.reset();
        EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
        NSDictionary *p = pipelined(cb);
        ASSERT_NE(p, nil);
        EXPECT_EQ([p[@"encoders"] intValue], 15);
        // all but what was recorded since the last hand-off (kHandOff) and the two encoders held for the look-ahead
        EXPECT_GE([p[@"pipelined"] intValue], 10) << [[p description] UTF8String];
        EXPECT_EQ([p[@"abandons"] intValue], 0);
        EXPECT_EQ([p[@"veto"] intValue], 0);
    }
    NSDictionary *after = counters(opened().device);
    const auto delta = [&](NSString *key) {
        return [after[key] longLongValue] - [before[key] longLongValue];
    };
    printf("hand-offs over 4 frames: %lld, %lld of them woke the worker\n", delta(@"pipeline_handoffs"),
           delta(@"pipeline_handoff_wakes"));
}

// Nine passes of eight commands hand the command buffer over at the ninth (kHandOff); with the worker asleep after it
// encoded them, a tenth publishes too little to hand it over again and wakes no one. Waiting for the encode ahead of it
// must still return: it hung on iOS 17 now and then in AnotherQueuesSubmissionTakesTheDeviceBack, whose worker slept
// before the tenth pass's publish.
TEST(GM20BFacadeCommit, AWaitForTheEncodeAheadWakesASleepingWorker)
{
    ASSERT_TRUE(opened().device != nil);
    // Let any pending 1 s idle release (which calls schedule.wake()) finish before recording so nothing else wakes it.
    for (int i = 0; i < 400 && gRM->idle.due(0); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < 9; p++)
        scale_frames::pass(cb, opened().state, into, shape, p, 0);
    EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
    for (int i = 0; i < 200 && !gRM->submitAsleep.load(std::memory_order_acquire); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    scale_frames::pass(cb, opened().state, into, shape, 9, 0);
    const auto waited = std::make_shared<std::promise<uint32_t>>();
    std::future<uint32_t> answer = waited->get_future();
    std::thread([cb, waited] {
        waited->set_value(scale_frames::waitAhead(cb));
    }).detach();
    const bool returned = answer.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(returned) << "the wait for the encode ahead slept through a publish that woke no worker";
    if (returned)
        EXPECT_EQ(answer.get(), 0u);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
}

TEST(GM20BFacadeCommit, PassesBesideAMemorylessColourAreEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    const id<MTLTexture> beside = scale_frames::scratch(opened().device, shape);
    ASSERT_NE(beside, nil);
    for (uint32_t frame = 0; frame < 4; frame++) {
        id<MTLCommandBuffer> cb = paced(queue, into, shape, frame, std::chrono::milliseconds(1), beside);
        EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(path(cb), "gm20b");
        NSDictionary *p = pipelined(cb);
        ASSERT_NE(p, nil);
        EXPECT_GE([p[@"pipelined"] intValue], 10) << [[p description] UTF8String];
        EXPECT_EQ([p[@"abandons"] intValue], 0);
        EXPECT_EQ([p[@"veto"] intValue], 0);
    }
}

// Pass 9 of 15 binds a buffer of another device, which Metal's validation stops, after the worker was handed (and may be
// encoding) the ones before it: the command buffer fails InvalidResource as it records, nothing of it runs, and the
// device comes back whole: the next command buffer is encoded ahead as any other.
TEST(GM20BFacadeCommit, AnEncoderFailedAfterTheHandOffsGivesTheDeviceBack)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        if (p == 9)
            scale_frames::pass(cb, opened().state, into, shape, p, 0, nil, ^(id<MTLRenderCommandEncoder> e) {
                [e setVertexBuffer:(id<MTLBuffer>)[ForeignBuffer new] offset:0 atIndex:5];
            });
        else
            scale_frames::pass(cb, opened().state, into, shape, p, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    [cb commit];
    [cb waitUntilCompleted];
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    printf("failed at pass 9: %s, path %s\n",
           [[p description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String, path(cb).c_str());
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInvalidResource);
    EXPECT_EQ(path(cb), "error:call.buf");
    EXPECT_NE([p[@"veto"] intValue] & int(VetoRefused), 0);
    EXPECT_EQ([p[@"pipelined"] intValue], 0);
    EXPECT_LE([p[@"abandons"] intValue], 1);
    id<MTLCommandBuffer> next = paced(queue, into, shape, 1, std::chrono::milliseconds(1));
    EXPECT_EQ(scale_frames::waitAhead(next), 0u);
    [next commit];
    [next waitUntilCompleted];
    ASSERT_EQ(next.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(next), "gm20b");
    NSDictionary *q = pipelined(next);
    ASSERT_NE(q, nil);
    EXPECT_GE([q[@"pipelined"] intValue], 10) << [[q description] UTF8String];
    EXPECT_EQ([q[@"abandons"] intValue], 0);
}

// Copy-engine launches in the model's jobs from `first` on: a fresh texture's fill is one (TegraGM20BMetalFresh.mm writeFresh)
size_t copyLaunches(size_t first)
{
    constexpr uint32_t kCopy = 4, kLaunchDma = 0x300;
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    size_t launches = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); j++) {
        const auto &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           method = (h & 0x1fff) * 4;
            if (op == 4) {
                launches += subc == kCopy && method == kLaunchDma;
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); k++, i++)
                launches += subc == kCopy && (op == 1        ? method + 4 * k
                                              : op == 5 && k ? method + 4
                                                             : method) == kLaunchDma;
        }
    }
    return launches;
}

id<MTLTexture> newTarget()
{
    return scale_frames::targets(opened().device, {1, 1, 1})[0];
}

// A new target its first pass clears and stores whole, with no renderTargetWidth set: Metal renders over the attachment
// whole, so nothing writes its contents first
// A job the kernel refuses fails its command buffer as Metal fails one its driver could not submit (Internal, the
// kernel's answer in the description), the path error:enc.kernel, and nothing of it replays on m2v; the next command
// buffer is raw again. Boot 22's iconservicesagent: `enc.kernel a=0 b=0` with no reason, then a 13.8 s m2v replay.
TEST(GM20BFacadeCommit, AJobTheKernelRefusesFailsItsCommandBufferAsMetalDoes)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{3, 6, 1};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLCommandBuffer> settled = scale_frames::record(queue, opened().state, into, shape, 0);
    [settled commit];
    [settled waitUntilCompleted];
    ASSERT_EQ(settled.status, MTLCommandBufferStatusCompleted);
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.refuse = 1000;
        kernel_model::model.refusal = kIOReturnBadArgument;
    }
    id<MTLCommandBuffer> cb = scale_frames::record(queue, opened().state, into, shape, 1);
    [cb commit];
    [cb waitUntilCompleted];
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.refuse = 0;
    }
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal);
    const std::string described = cb.error.localizedDescription.UTF8String ?: "";
    EXPECT_NE(described.find("0xe00002c2"), std::string::npos) << described;
    EXPECT_EQ(path(cb), "error:enc.kernel");
    id<MTLCommandBuffer> after = scale_frames::record(queue, opened().state, into, shape, 2);
    [after commit];
    [after waitUntilCompleted];
    EXPECT_EQ(after.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(after), "gm20b");
}

TEST(GM20BFacadeCommit, ANewTargetClearedWholeTakesNoFill)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const size_t first = jobCount();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    scale_frames::pass(cb, opened().state, {newTarget()}, {1, 6, 1}, 0, 0);
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(copyLaunches(first), 0u);
}

// Each frame draws into two new targets, as the render server's close animations do: one cleared whole (no fill), one
// loaded first (its contents written ahead of the pass that loads it, in a blit of their own)
TEST(GM20BFacadeCommit, NewTargetsEachFrameAreEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    for (uint32_t frame = 0; frame < 4; frame++) {
        const size_t first = jobCount();
        const id<MTLTexture> cleared = newTarget(), loaded = newTarget();
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (int p = 0; p < shape.passes; p++) {
            if (p == 2)
                scale_frames::pass(cb, opened().state, {cleared}, {1, 6, 1}, 0, frame);
            else if (p == 5)
                scale_frames::pass(cb, opened().state, {loaded}, {1, 6, 1}, 1, frame);
            else
                scale_frames::pass(cb, opened().state, into, shape, p, frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
        NSDictionary *p = pipelined(cb);
        ASSERT_NE(p, nil);
        printf("two new targets, frame %u: %d of %d encoders encoded ahead, %zu copy launches\n", frame,
               [p[@"pipelined"] intValue], [p[@"encoders"] intValue], copyLaunches(first));
        EXPECT_GE([p[@"pipelined"] intValue], [p[@"encoders"] intValue] - 5) << [[p description] UTF8String];
        EXPECT_EQ([p[@"abandons"] intValue], 0);
        EXPECT_GE(copyLaunches(first), 1u);
    }
}

TEST(GM20BFacadeCommit, AnotherQueuesSubmissionTakesTheDeviceBack)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> recorded = [opened().device newCommandQueue], other = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4}, small{1, 6, 1};
    const auto into = scale_frames::targets(opened().device, shape);
    const auto elsewhere = scale_frames::targets(opened().device, small);
    id<MTLCommandBuffer> cb = [recorded commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        scale_frames::pass(cb, opened().state, into, shape, p, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (p == 9) {
            EXPECT_EQ(scale_frames::waitAhead(cb), 0u) << "the worker holds the encode ahead the submission takes back";
            id<MTLCommandBuffer> between = scale_frames::record(other, opened().state, elsewhere, small, 0);
            [between commit];
            [between waitUntilCompleted];
            EXPECT_EQ(between.status, MTLCommandBufferStatusCompleted);
        }
    }
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(cb), "gm20b");
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"abandons"] intValue], 1) << [[p description] UTF8String];
    EXPECT_EQ([p[@"pipelined"] intValue], 0);
}

// A compute encoder after pass 12 of 15, which an encode ahead does not take: the encoders before it are encoded ahead
// (they go to the GPU at the commit's turn as a range of their own), the compute encoder and the rest at the commit
TEST(GM20BFacadeCommit, EncodersBeforeAComputeEncoderAreEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        scale_frames::pass(cb, opened().state, into, shape, p, 0);
        if (p == 11)
            [[cb computeCommandEncoder] endEncoding];
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"encoders"] intValue], 16);
    EXPECT_GE([p[@"pipelined"] intValue], 9) << [[p description] UTF8String];
    EXPECT_LE([p[@"pipelined"] intValue], 12) << [[p description] UTF8String];
    EXPECT_NE([p[@"stopped"] unsignedIntValue] & VetoOps, 0u) << [[p description] UTF8String];
    EXPECT_EQ([p[@"veto"] intValue], 0);
    EXPECT_EQ([p[@"abandons"] intValue], 0);
}

TEST(GM20BFacadeCommit, EmptyAndNearEmptyCommandBuffersBypassTheSubmitWorker)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    id<MTLTexture> target = newTarget();
    id<MTLBuffer> buf = [opened().device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(target != nil && buf != nil);
    const size_t jobsBefore = jobCount();
#if SWITCH_METAL_DIAGNOSTICS
    const uint64_t deferredBefore = gRM->stats.deferred.load();
#endif
    {
        scale_frames::WorkerHold hold;
        auto runBounded = [&](id<MTLCommandBuffer> cb) {
            auto done = std::make_shared<std::promise<void>>();
            std::future<void> waited = done->get_future();
            std::thread worker([cb, done] {
                [cb commit];
                [cb waitUntilCompleted];
                done->set_value();
            });
            const bool ok = waited.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            if (!ok)
                hold.release();
            worker.join();
            return ok;
        };
        id<MTLCommandBuffer> empty = [queue commandBuffer];
        ASSERT_TRUE(runBounded(empty)) << "empty command buffer waited on the held submit worker";
        EXPECT_EQ(empty.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(path(empty), "gm20b");

        id<MTLCommandBuffer> emptyBlit = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [emptyBlit blitCommandEncoder];
        [blit synchronizeResource:buf];
        [blit optimizeContentsForGPUAccess:target];
        [blit endEncoding];
        ASSERT_TRUE(runBounded(emptyBlit)) << "no-op blit command buffer waited on the held submit worker";
        EXPECT_EQ(emptyBlit.status, MTLCommandBufferStatusCompleted);

        id<MTLCommandBuffer> emptyCompute = [queue commandBuffer];
        [[emptyCompute computeCommandEncoder] endEncoding];
        ASSERT_TRUE(runBounded(emptyCompute)) << "empty compute command buffer waited on the held submit worker";
        EXPECT_EQ(emptyCompute.status, MTLCommandBufferStatusCompleted);

        for (MTLLoadAction load : {MTLLoadActionDontCare, MTLLoadActionLoad}) {
            id<MTLCommandBuffer> noOpRender = [queue commandBuffer];
            MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
            d.colorAttachments[0].texture = target;
            d.colorAttachments[0].loadAction = load;
            d.colorAttachments[0].storeAction = MTLStoreActionDontCare;
            [[noOpRender renderCommandEncoderWithDescriptor:d] endEncoding];
            ASSERT_TRUE(runBounded(noOpRender)) << "no-op render command buffer waited on the held submit worker";
            EXPECT_EQ(noOpRender.status, MTLCommandBufferStatusCompleted);
        }

        std::mutex lock;
        std::vector<int> events;
        std::atomic<int> presents{0};
        EmptyFastPathDrawable *drawable = [EmptyFastPathDrawable new];
        drawable.presents = &presents;
        id<MTLCommandBuffer> withHandlers = [queue commandBuffer];
        std::mutex *const lockPtr = &lock;
        std::vector<int> *const eventsPtr = &events;
        [withHandlers addScheduledHandler:^(id<MTLCommandBuffer>) {
            std::lock_guard<std::mutex> l(*lockPtr);
            eventsPtr->push_back(1);
        }];
        [withHandlers presentDrawable:drawable];
        [withHandlers addCompletedHandler:^(id<MTLCommandBuffer>) {
            std::lock_guard<std::mutex> l(*lockPtr);
            eventsPtr->push_back(2);
        }];
        ASSERT_TRUE(runBounded(withHandlers)) << "empty command buffer with handlers waited on the held submit worker";
        EXPECT_EQ(withHandlers.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(presents.load(std::memory_order_acquire), 1);
        std::lock_guard<std::mutex> l(lock);
        EXPECT_EQ(events, (std::vector<int>{1, 2}));
    }
    EXPECT_EQ(jobCount(), jobsBefore);
#if SWITCH_METAL_DIAGNOSTICS
    EXPECT_EQ(gRM->stats.deferred.load(), deferredBefore);
#endif

    id<MTLCommandBuffer> clearOnly = [queue commandBuffer];
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = target;
    d.colorAttachments[0].loadAction = MTLLoadActionClear;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[clearOnly renderCommandEncoderWithDescriptor:d] endEncoding];
    [clearOnly commit];
    [clearOnly waitUntilCompleted];
    EXPECT_EQ(clearOnly.status, MTLCommandBufferStatusCompleted);
    EXPECT_GT(jobCount(), jobsBefore);
}

TEST(GM20BFacadeCommit, AnEmptyCommandBufferOnAnotherQueueDoesNotAbandonAnEncodeAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> recorded = [opened().device newCommandQueue], other = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLCommandBuffer> cb = [recorded commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        scale_frames::pass(cb, opened().state, into, shape, p, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (p == 9) {
            EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
            id<MTLCommandBuffer> empty = [other commandBuffer];
            [empty commit];
            [empty waitUntilCompleted];
            EXPECT_EQ(empty.status, MTLCommandBufferStatusCompleted);
            id<MTLCommandBuffer> emptyBlit = [other commandBuffer];
            [[emptyBlit blitCommandEncoder] endEncoding];
            [emptyBlit commit];
            [emptyBlit waitUntilCompleted];
            EXPECT_EQ(emptyBlit.status, MTLCommandBufferStatusCompleted);
        }
    }
    EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"abandons"] intValue], 0) << [[p description] UTF8String];
    EXPECT_GE([p[@"pipelined"] intValue], 10) << [[p description] UTF8String];
}

TEST(GM20BFacadeCommit, ALongUnsplitCommandBufferKicksInTwoToThreeRanges)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape medium{15, 90, 4}, large{24, 144, 4}, small{3, 12, 2};
    const auto into = scale_frames::targets(opened().device, medium);
    const auto rawCount = [&] {
        return [counters(opened().device)[@"gm20b"] unsignedLongLongValue];
    };
    {
        id<MTLCommandBuffer> warm = scale_frames::record(queue, opened().state, into, small, 0);
        [warm commit];
        [warm waitUntilCompleted];
        ASSERT_EQ(warm.status, MTLCommandBufferStatusCompleted);
    }
    const auto runUnsplit = [&](const scale_frames::Shape &shape, uint32_t frame) {
        const uint64_t before = rawCount();
        id<MTLCommandBuffer> cb;
        {
            scale_frames::WorkerHold hold;
            cb = scale_frames::record(queue, opened().state, into, shape, frame);
            [cb commit];
        }
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(path(cb), "gm20b");
        return rawCount() - before;
    };
    EXPECT_EQ(runUnsplit(small, 1), 1u);
    const uint64_t mediumRanges = runUnsplit(medium, 2);
    EXPECT_GE(mediumRanges, 2u);
    EXPECT_LE(mediumRanges, 3u);
    const uint64_t largeRanges = runUnsplit(large, 3);
    EXPECT_GE(largeRanges, 2u);
    EXPECT_LE(largeRanges, 3u);

    id<MTLCommandBuffer> earlyStop = [queue commandBuffer];
    for (int p = 0; p < medium.passes; p++) {
        scale_frames::pass(earlyStop, opened().state, into, medium, p, 4);
        if (p == 2)
            [[earlyStop computeCommandEncoder] endEncoding];
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(scale_frames::waitAhead(earlyStop), 0u);
    const uint64_t beforeEarly = rawCount();
    [earlyStop commit];
    [earlyStop waitUntilCompleted];
    ASSERT_EQ(earlyStop.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(earlyStop), "gm20b");
    const uint64_t earlyRanges = rawCount() - beforeEarly;
    EXPECT_EQ(earlyRanges, 3u);
    NSDictionary *p = pipelined(earlyStop);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"abandons"] intValue], 0);
    EXPECT_NE([p[@"stopped"] unsignedIntValue] & VetoOps, 0u) << [[p description] UTF8String];
}

struct Latency {
    std::vector<double> commit, scheduled, firstSubmit;
};

double us(Clock::duration d)
{
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()) / 1000.0;
}

// Frames of `shape` at 60 Hz with the GPU taking `gpuUs` for each frame's jobs and up to three frames in flight: each
// recorded at the render thread's pace, committed, waited for scheduled as Core Animation does before its swap
Latency frames(const scale_frames::Shape &shape, int gpuUs, int count)
{
    timed_gpu::Gpu gpu;
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const auto into = scale_frames::targets(opened().device, shape);
    const auto pace = std::chrono::microseconds(6000 / std::max(shape.passes, 1));
    {
        id<MTLCommandBuffer> warm = scale_frames::record(queue, opened().state, into, shape, 0);
        [warm commit];
        [warm waitUntilCompleted];
        gpu.setPerJob(std::chrono::microseconds(gpuUs / int(std::max<size_t>(gpu.submitted(), 1))));
    }
    std::vector<id<MTLCommandBuffer>> inFlight;
    Latency out;
    auto next = Clock::now();
    for (int frame = 0; frame < count; frame++) {
        if (inFlight.size() == 3) {
            [inFlight.front() waitUntilCompleted];
            inFlight.erase(inFlight.begin());
        }
        id<MTLCommandBuffer> cb = paced(queue, into, shape, uint32_t(frame), pace);
        const size_t before = gpu.submitted();
        const auto committing = Clock::now();
        [cb commit];
        const auto returned = Clock::now();
        [cb waitUntilScheduled];
        const auto scheduled = Clock::now();
        for (int spin = 0; spin < 2000 && gpu.submitted() <= before; spin++)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        out.commit.push_back(us(returned - committing));
        out.scheduled.push_back(us(scheduled - returned));
        out.firstSubmit.push_back(us(gpu.submittedAt(before) - committing));
        inFlight.push_back(cb);
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
    for (id<MTLCommandBuffer> cb : inFlight)
        [cb waitUntilCompleted];
    return out;
}

uint64_t threadNs()
{
    return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
}

// The caller's CPU recording a 15-pass, 90-draw command buffer (its encoders, not its commit), as many after it as
// before, the worker encoding ahead beside it: what the tape's publication and its growth cost the recording thread
TEST(GM20BFacadeCommit, RecordingCost)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    std::vector<double> recordUs, commitUs;
    for (uint32_t frame = 0; frame < 60; frame++) {
        const uint64_t began = threadNs();
        id<MTLCommandBuffer> cb = scale_frames::record(queue, opened().state, into, shape, frame);
        const uint64_t recorded = threadNs();
        [cb commit];
        const uint64_t committed = threadNs();
        [cb waitUntilCompleted];
        if (frame < 20)
            continue;
        recordUs.push_back(double(recorded - began) / 1000.0);
        commitUs.push_back(double(committed - recorded) / 1000.0);
    }
    char line[200];
    snprintf(line, sizeof line, "recording 15 passes 90 draws: thread cpu p50 %.1f p90 %.1f us, commit p50 %.1f us\n",
             percentileOf(recordUs, 0.5), percentileOf(recordUs, 0.9), percentileOf(commitUs, 0.5));
    std::ofstream(outputs() + "/recording_cost.txt") << line;
    printf("%s", line);
}

TEST(GM20BFacadeCommit, GiveBackDropsUncommittedEncodeAheadAndPurgesVolatileHeap)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{5, 30, 2};
    const auto into = scale_frames::targets(opened().device, shape);

    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.size = 256 << 10;
    hd.storageMode = MTLStorageModeShared;
    hd.type = MTLHeapTypeAutomatic;
    id<MTLHeap> heap = [opened().device newHeapWithDescriptor:hd];
    ASSERT_NE(heap, nil);
    const uint32_t arena = ((SwitchMetalHeap *)heap)->_arena - 1;
    ASSERT_EQ([heap setPurgeableState:MTLPurgeableStateVolatile], MTLPurgeableStateNonVolatile);

    @autoreleasepool {
        id<MTLCommandBuffer> orphan = [queue commandBuffer];
        for (int p = 0; p < shape.passes; p++)
            scale_frames::pass(orphan, opened().state, into, shape, p, 0);
        EXPECT_EQ(scale_frames::waitAhead(orphan), 0u);
        ASSERT_EQ(gRM->committed.load(), 1u);
    }
    gRawRelease.release();
    EXPECT_EQ(gRM->committed.load(), 0u);
    EXPECT_TRUE(gRM->raw->arenaPurged(arena));

    EXPECT_EQ([heap setPurgeableState:MTLPurgeableStateNonVolatile], MTLPurgeableStateEmpty);
    ASSERT_FALSE(gRM->raw->arenaPurged(arena));
    EXPECT_EQ([heap setPurgeableState:MTLPurgeableStateVolatile], MTLPurgeableStateNonVolatile);

    id<MTLCommandBuffer> live = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++)
        scale_frames::pass(live, opened().state, into, shape, p, 1);
    EXPECT_EQ(scale_frames::waitAhead(live), 0u);
    ASSERT_EQ(gRM->committed.load(), 1u);
    gRawRelease.release();
    EXPECT_EQ(gRM->committed.load(), 0u);
    EXPECT_TRUE(gRM->raw->arenaPurged(arena));
    [live commit];
    [live waitUntilCompleted];
    ASSERT_EQ(live.status, MTLCommandBufferStatusCompleted);
    heap = nil;

    id<MTLCommandBuffer> kept = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++)
        scale_frames::pass(kept, opened().state, into, shape, p, 2);
    EXPECT_EQ(scale_frames::waitAhead(kept), 0u);
    ASSERT_EQ(gRM->committed.load(), 1u);
    gRawRelease.release();
    EXPECT_EQ(gRM->committed.load(), 1u);
    [kept commit];
    [kept waitUntilCompleted];
    ASSERT_EQ(kept.status, MTLCommandBufferStatusCompleted);
    NSDictionary *p = pipelined(kept);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"abandons"] intValue], 0) << [[p description] UTF8String];
    EXPECT_GE([p[@"pipelined"] intValue], 1) << [[p description] UTF8String];
}

TEST(GM20BFacadeCommit, PostFirstDrawVertexBytesShadowInCachedMemory)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const auto into = scale_frames::targets(opened().device, {1, 1, 1});
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = into[0];
    d.colorAttachments[0].loadAction = MTLLoadActionClear;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
    [e setRenderPipelineState:opened().state];

    const float v0a[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
    const float m2a[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float frag[4] = {1, 0, 0, 1};
    [e setVertexBytes:v0a length:sizeof v0a atIndex:0];
    [e setVertexBytes:m2a length:sizeof m2a atIndex:2];
    [e setFragmentBytes:frag length:sizeof frag atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

    const float v0b[16] = {0, 0, 0, 1, 0.5f, 0, 0, 1, 0, 0.5f, 0, 1, 0.5f, 0.5f, 0, 1};
    uint8_t v1b[288];
    for (size_t i = 0; i < sizeof v1b; ++i)
        v1b[i] = uint8_t(i ^ 0x5a);
    const float m2b[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float v3b[16] = {3, 3, 3, 3};
    [e setVertexBytes:v0b length:sizeof v0b atIndex:0];
    [e setVertexBytes:v1b length:sizeof v1b atIndex:1];
    [e setVertexBytes:m2b length:sizeof m2b atIndex:2];
    [e setVertexBytes:v3b length:sizeof v3b atIndex:3];
    [e setFragmentBytes:frag length:sizeof frag atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

    std::vector<uint8_t> big(1028, 0x3c);
    [e setVertexBytes:big.data() length:big.size() atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

    SwitchMetalCommandBuffer *raw = (SwitchMetalCommandBuffer *)cb;
    std::vector<size_t> drawOps;
    for (size_t i = 0; i < raw->_plan.size(); ++i)
        if (raw->_plan.operation(i).op == OpDraw)
            drawOps.push_back(i);
    ASSERT_EQ(drawOps.size(), 3u);

    const auto &state1 = raw->_plan.renderStates[raw->_plan.draw(drawOps[1]).state];
    const gm20b::BufferBinding &b0 = raw->_plan.buffers[0][state1.buffers[0]][0];
    const gm20b::BufferBinding &b1 = raw->_plan.buffers[0][state1.buffers[0]][1];
    const gm20b::BufferBinding &b2 = raw->_plan.buffers[0][state1.buffers[0]][2];
    const gm20b::BufferBinding &b3 = raw->_plan.buffers[0][state1.buffers[0]][3];
    const gm20b::BufferBinding &bf = raw->_plan.buffers[1][state1.buffers[1]][0];
    ASSERT_NE(b0.shadow, 0u);
    ASSERT_NE(b1.shadow, 0u);
    ASSERT_NE(b2.shadow, 0u);
    EXPECT_EQ(b3.shadow, 0u);
    EXPECT_EQ(bf.shadow, 0u);
    EXPECT_EQ(memcmp(raw->_shadow.ownData() + b0.shadow - 1, v0b, sizeof v0b), 0);
    EXPECT_EQ(memcmp(raw->_shadow.ownData() + b1.shadow - 1, v1b, sizeof v1b), 0);
    EXPECT_EQ(memcmp(raw->_shadow.ownData() + b2.shadow - 1, m2b, sizeof m2b), 0);

    const auto &state2 = raw->_plan.renderStates[raw->_plan.draw(drawOps[2]).state];
    EXPECT_EQ(raw->_plan.buffers[0][state2.buffers[0]][1].shadow, 0u);

    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
}

TEST(GM20BFacadeCommit, CommitLatency)
{
    ASSERT_TRUE(opened().device != nil);
    std::ofstream report(outputs() + "/commit_latency.txt");
    const scale_frames::Shape shapes[] = {{1, 6, 1}, {15, 90, 4}};
    for (const auto &shape : shapes)
        for (const int gpuUs : {2000, 16000}) {
            const Latency l = frames(shape, gpuUs, 24);
            char line[320];
            snprintf(line, sizeof line,
                     "passes %d draws %d gpu %d us: commit p50 %.1f p90 %.1f us, waitUntilScheduled p50 %.1f p90 %.1f "
                     "us, commit to first job p50 %.1f p90 %.1f us\n",
                     shape.passes, shape.draws, gpuUs, percentileOf(l.commit, 0.5), percentileOf(l.commit, 0.9),
                     percentileOf(l.scheduled, 0.5), percentileOf(l.scheduled, 0.9), percentileOf(l.firstSubmit, 0.5),
                     percentileOf(l.firstSubmit, 0.9));
            report << line;
            printf("%s", line);
        }
}

}  // namespace
