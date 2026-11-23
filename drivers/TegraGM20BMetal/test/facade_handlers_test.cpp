// SPDX-License-Identifier: MIT
// What a raw command buffer's scheduled and completed stages promise its caller, with the submit worker signalling
// scheduled before it hands the range to the completion thread (TegraGM20BMetalSubmit.mm): scheduled handlers before
// completed ones and each kind in queue order, and a waitUntilCompleted right after waitUntilScheduled woken by the
// range it waits for, while other work keeps the worker from going idle (an idle worker would wake the completion
// thread on its own and hide a lost wake), measured draws' timings handed over and learned on the submit worker alone
// while the completion thread finishes the command buffers before them, and another thread's move of a texture's memory
// run by the submit worker between its submissions, and a command buffer freed on its recording thread after its
// completion let go of it, made again there (TegraGM20BMetalCommandBuffer.mm commandBufferDropped).
#include "scale_frames.h"
#include "../stamps.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

#include <gtest/gtest.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using scale_frames::opened;
using Clock = std::chrono::steady_clock;

TEST(GM20BFacadeHandlers, ScheduledHandlersRunBeforeCompletedOnesInQueueOrder)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{3, 18, 2};
    const auto into = scale_frames::targets(opened().device, shape);
    std::mutex lock;
    std::vector<int> order;  // i + 1 where buffer i's scheduled handler ran, -(i + 1) its completed one
    std::mutex *const handlersLock = &lock;
    std::vector<int> *const handlersOrder = &order;
    NSMutableArray *kept = [NSMutableArray array];
    for (int i = 0; i < 24; i++) {
        id<MTLCommandBuffer> cb = scale_frames::record(queue, opened().state, into, shape, uint32_t(i));
        [cb addScheduledHandler:^(id<MTLCommandBuffer>) {
            std::lock_guard<std::mutex> l(*handlersLock);
            handlersOrder->push_back(i + 1);
        }];
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
            std::lock_guard<std::mutex> l(*handlersLock);
            handlersOrder->push_back(-(i + 1));
        }];
        [cb commit];
        [kept addObject:cb];
    }
    [kept.lastObject waitUntilCompleted];
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard<std::mutex> l(lock);
    ASSERT_EQ(order.size(), 48u);
    int scheduled = 0, completed = 0;
    std::vector<bool> seen(25, false);
    for (int at : order) {
        if (at > 0) {
            EXPECT_EQ(at, ++scheduled);
            seen[size_t(at)] = true;
        } else {
            EXPECT_EQ(-at, ++completed);
            EXPECT_TRUE(seen[size_t(-at)]) << "buffer " << -at - 1 << " completed before it was scheduled";
        }
    }
}

TEST(GM20BFacadeHandlers, AWaitForCompletionAfterScheduledIsWokenByItsRange)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue], busy = [opened().device newCommandQueue];
    const scale_frames::Shape shape{2, 6, 2};
    const auto into = scale_frames::targets(opened().device, shape),
               elsewhere = scale_frames::targets(opened().device, shape);
    std::atomic<bool> stop{false};
    NSMutableArray *kept = [NSMutableArray array], *keptBusy = [NSMutableArray array];
    std::thread other([&] {
        for (uint32_t frame = 0; !stop.load(std::memory_order_relaxed); frame++)
            @autoreleasepool {
                id<MTLCommandBuffer> cb = scale_frames::record(busy, opened().state, elsewhere, shape, frame);
                [cb commit];
                [keptBusy addObject:cb];
            }
    });
    double longest = 0;
    for (uint32_t frame = 0; frame < 300; frame++)
        @autoreleasepool {
            id<MTLCommandBuffer> waited = scale_frames::record(queue, opened().state, into, shape, frame);
            id<MTLCommandBuffer> behind = scale_frames::record(queue, opened().state, into, shape, frame);
            [waited commit];
            [behind commit];
            [kept addObject:waited];
            [kept addObject:behind];
            [waited waitUntilScheduled];
            const auto began = Clock::now();
            [waited waitUntilCompleted];
            longest = std::max(longest, std::chrono::duration<double, std::milli>(Clock::now() - began).count());
            EXPECT_EQ(waited.status, MTLCommandBufferStatusCompleted);
        }
    stop = true;
    other.join();
    [keptBusy.lastObject waitUntilCompleted];
    [kept.lastObject waitUntilCompleted];
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("longest waitUntilCompleted after waitUntilScheduled: %.2f ms\n", longest);
    EXPECT_LT(longest, 250.0);
}

// A client's draws alone in their passes over a whole target are measured (band_rates.h): each commit hands its
// timings to the device and the next begin learns them, both on the submit worker, while the completion thread finishes the
// command buffers committed before (thread-sanitized: nothing else touches the timings)
TEST(GM20BFacadeHandlers, MeasuredDrawsTimingsStayTheSubmitWorkersBesideItsCompletions)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{1, 1, 1};
    const auto into = scale_frames::targets(opened().device, shape);
    size_t first;
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        first = kernel_model::model.jobs.size();
    }
    NSMutableArray *kept = [NSMutableArray array];
    for (uint32_t frame = 0; frame < 160; frame++)
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
            d.colorAttachments[0].texture = into[0];
            d.colorAttachments[0].loadAction = MTLLoadActionLoad;
            d.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
            [e setRenderPipelineState:opened().state];
            const float vertices[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
            const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            const float colour[4] = {float(frame % 5) / 5, 0.5f, 0.5f, 1};
            [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
            [e setVertexBytes:transform length:sizeof transform atIndex:2];
            [e setFragmentBytes:colour length:sizeof colour atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [e endEncoding];
            [cb commit];
            [kept addObject:cb];
        }
    [kept.lastObject waitUntilCompleted];
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (id<MTLCommandBuffer> cb in kept)
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    uint32_t stamps = 0;
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    for (size_t j = first; j < kernel_model::model.jobs.size(); j++) {
        const std::vector<uint32_t> &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> m(words.size());
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        for (long i = 0; i < n; i++)
            stamps +=
                m[size_t(i)].method == NVB197_SET_REPORT_SEMAPHORE_D && m[size_t(i)].value == gm20b::kDrawStampRelease;
    }
    EXPECT_GE(stamps, 2 * gm20b::kWarmSamples) << "no draw was measured: the test reaches no timing";
}

// Another thread's moves (gm20bExcludingSubmissions: a compressed texture made plain, a fresh texture's fill before its CPU
// copy) while command buffers are committed back to back: each runs on the submit worker, between its submissions
TEST(GM20BFacadeHandlers, AnotherThreadsMoveRunsOnTheSubmitWorker)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{2, 12, 2};
    const auto into = scale_frames::targets(opened().device, shape);
    constexpr int kMoves = 50;
    std::atomic<bool> stop{false};
    NSMutableArray *kept = [NSMutableArray array];
    std::thread committer([&] {
        for (uint32_t frame = 0; !stop.load(std::memory_order_relaxed); frame++)
            @autoreleasepool {
                id<MTLCommandBuffer> cb = scale_frames::record(queue, opened().state, into, shape, frame);
                [cb commit];
                [kept addObject:cb];
                [cb waitUntilScheduled];
            }
    });
    int onWorker = 0;
    std::thread mover([&] {
        for (int i = 0; i < kMoves; i++) {
            char name[64] = "";
            gm20bExcludingSubmissions([&] {
                pthread_getname_np(pthread_self(), name, sizeof name);
                return true;
            });
            onWorker += strcmp(name, "com.brunostjohn.switch.gm20b.submit") == 0;
        }
    });
    mover.join();
    stop = true;
    committer.join();
    [kept.lastObject waitUntilCompleted];
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(onWorker, kMoves) << "a move ran on the thread that asked for it";
}

pthread_t gSentinelFreedOn = nullptr;

}  // namespace

// Its dealloc says on which thread the object it is associated with went
@interface SwitchMetalFreedOnSentinel : NSObject
@end

@implementation SwitchMetalFreedOnSentinel

- (void)dealloc
{
    gSentinelFreedOn = pthread_self();
}

@end

namespace {

// A command buffer whose last reference goes on the thread that recorded it, after the completion thread let go of it,
// gives its block to the next command buffer made there. The completion's last write to it, its turn's mark, comes
// after the waitUntilCompleted wake; what orders the dealloc and the reuse after that write is libobjc's reference
// count, which the sanitizer does not see, and here a Mach semaphore, which it does not model either: only the command
// buffer's own atomic, read in its dealloc, shows it that order, every time
TEST(GM20BFacadeHandlers, ACommandBufferFreedAfterItsCompletionLetGoIsMadeAgainAfterThatCompletion)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> first = [opened().device newCommandQueue], second = [opened().device newCommandQueue];
    const scale_frames::Shape shape{1, 6, 1};
    const auto into = scale_frames::targets(opened().device, shape);
    semaphore_t letGo = MACH_PORT_NULL;
    ASSERT_EQ(semaphore_create(mach_task_self(), &letGo, SYNC_POLICY_FIFO, 0), KERN_SUCCESS);
    static char sentinelKey;
    id<MTLCommandBuffer> done = nil, after = nil;
    @autoreleasepool {
        done = scale_frames::record(first, opened().state, into, shape, 0);
        objc_setAssociatedObject(done, &sentinelKey, [SwitchMetalFreedOnSentinel new],
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [done commit];
        [done waitUntilCompleted];
        // the completion thread takes `after` once it is through with `done`; its handler runs after that. Kept to the
        // end, so no other command buffer's block goes back to the pool meanwhile
        after = scale_frames::record(second, opened().state, into, shape, 1);
        [after addCompletedHandler:^(id<MTLCommandBuffer>) {
            semaphore_signal(letGo);
        }];
        [after commit];
    }
    ASSERT_EQ(semaphore_wait(letGo), KERN_SUCCESS);
    const void *block = (__bridge const void *)done;
    gSentinelFreedOn = nullptr;
    done = nil;
    ASSERT_EQ(gSentinelFreedOn, pthread_self()) << "the last reference went here";
    @autoreleasepool {
        id<MTLCommandBuffer> next = scale_frames::record(first, opened().state, into, shape, 2);
        EXPECT_EQ((__bridge const void *)next, block) << "the pool hands the block back";
        [next commit];
        [next waitUntilCompleted];
        EXPECT_EQ(next.status, MTLCommandBufferStatusCompleted);
    }
    [after waitUntilCompleted];
    after = nil;
    semaphore_destroy(mach_task_self(), letGo);
}

}  // namespace
