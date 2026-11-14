// SPDX-License-Identifier: MIT
// What a raw command buffer's scheduled and completed stages promise its caller, with the submit worker signalling
// scheduled before it hands the range to the completion thread (RawMetalSubmit.mm): scheduled handlers before
// completed ones and each kind in queue order, and a waitUntilCompleted right after waitUntilScheduled woken by the
// range it waits for, while other work keeps the worker from going idle (an idle worker would wake the completion
// thread on its own and hide a lost wake). Every command buffer is kept until all completed: libobjc's release orders a
// dealloc after the completion thread's last touch where TSAN cannot see it.
#include "raw_scale_frames.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using scale_frames::opened;
using Clock = std::chrono::steady_clock;

TEST(RawFacadeHandlers, ScheduledHandlersRunBeforeCompletedOnesInQueueOrder)
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

TEST(RawFacadeHandlers, AWaitForCompletionAfterScheduledIsWokenByItsRange)
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

}  // namespace
