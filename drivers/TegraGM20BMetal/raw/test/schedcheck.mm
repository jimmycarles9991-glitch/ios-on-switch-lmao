// SPDX-License-Identifier: MIT
// raw GM20B path: a command buffer's scheduled handlers run at the priority of the thread that commits and then waits in
// waitUntilScheduled, as Core Animation's fixed-priority render thread does every frame. The committing thread here takes
// SCHED_FIFO at `priority`; each handler records its own thread's policy and priority, and every one must match. A handler on a
// QoS queue's worker would run timeshare, which a daemon's clamp puts below the waiting thread. Also prints how long the
// waits blocked (the rates line's ws=); SCHEDCHECK_BUSY=<n> keeps n user-interactive threads spinning meanwhile, the
// contention a swipe puts on the cores.
// usage: schedcheck <pack> <cache> <nvk host dylib> [command buffers] [priority]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <atomic>
#include <thread>
#include <vector>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: schedcheck <pack> <cache> <nvk dylib> [command buffers] [priority]\n");
            return 2;
        }
        const unsigned count = argc > 4 ? unsigned(atoi(argv[4])) : 60;
        const int priority = argc > 5 ? atoi(argv[5]) : 47;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", why.UTF8String ?: "m2v");
            return 1;
        }
        sched_param param{};
        param.sched_priority = priority;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            fprintf(stderr, "cannot take SCHED_FIFO %d here\n", priority);
            return 2;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> a = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        std::atomic<bool> spin{true};
        std::vector<std::thread> busy;
        for (int n = getenv("SCHEDCHECK_BUSY") ? atoi(getenv("SCHEDCHECK_BUSY")) : 0; n > 0; n--)
            busy.emplace_back([&spin] {
                pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
                while (spin.load(std::memory_order_relaxed)) {
                }
            });
        std::atomic<unsigned> ranCount{0}, matchedCount{0};
        std::vector<int> seenBy(count, -1);
        std::atomic<unsigned> *ran = &ranCount, *matched = &matchedCount;
        int *seen = seenBy.data();
        for (unsigned i = 0; i < count; i++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:4096];
            [e endEncoding];
            [cb addScheduledHandler:^(id<MTLCommandBuffer>) {
                int policy = 0;
                sched_param p{};
                pthread_getschedparam(pthread_self(), &policy, &p);
                seen[i] = policy == SCHED_FIFO ? p.sched_priority : -p.sched_priority;
                ran->fetch_add(1);
                if (policy == SCHED_FIFO && p.sched_priority == priority)
                    matched->fetch_add(1);
            }];
            [cb commit];
            [cb waitUntilScheduled];
            if (i + 1 == count)
                [cb waitUntilCompleted];
        }
        spin = false;
        for (std::thread &t : busy)
            t.join();
        char line[1024];
        raw_metal_rates(line, sizeof line);
        printf("handlers %u of %u ran, %u at SCHED_FIFO %d (first %d, last %d; negative: timeshare at that priority)\n",
               ran->load(), count, matched->load(), priority, seen[0], seen[count - 1]);
        printf("rates: %s\n", line);
        return ran->load() == count && matched->load() == count ? 0 : 1;
    }
}
