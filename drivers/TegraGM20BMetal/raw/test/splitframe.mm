// SPDX-License-Identifier: MIT
// Command buffers the facade submits in segments (planSegments): each one copies a pattern down a chain of raw buffers through a
// blit encoder that only m2v takes (a copy through an m2v buffer of its own, call.bufcopy), between raw copies, so its data
// crosses raw -> m2v -> raw inside one command buffer (SPLITFRAME_ORDER=m2v-raw: m2v -> raw; raw: all of it raw). Committed
// back to back from SPLITFRAME_THREADS threads (Core Animation's render thread and the others that commit), each waiting until
// scheduled as Core Animation's render thread does, with scheduled and completed handlers that ask the raw objects for their m2v
// twins (Core Animation's handlers touch the textures of the frame). Over the mock's GPU (MOCK_GPU_US; 0 races the completion
// thread against the submitting one) every chain must arrive intact and every handler run, each command buffer's scheduled
// ones before its completed ones; ordercheck.py reads MOCK_GPU_LOG for the order on the GPU.
// usage: NVK_COPIES=ce splitframe <pack> <cache> <nvk host dylib> [command buffers per thread] (NVK_COPIES=ce: m2v's copies on
// the copy engine, which the mock runs; its shader copies it does not, and every chain through m2v would read as broken)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <memory>
#include <cstring>
#include <thread>
#include <vector>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static double nowMs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
}

struct Chain {
    std::vector<id<MTLBuffer>> links, a, b;
};

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: splitframe <pack> <cache> <nvk dylib> [command buffers per thread]\n");
            return 2;
        }
        const unsigned count = argc > 4 ? unsigned(atoi(argv[4])) : 24;
        const unsigned threads = getenv("SPLITFRAME_THREADS") ? unsigned(atoi(getenv("SPLITFRAME_THREADS"))) : 2;
        const char *order = getenv("SPLITFRAME_ORDER") ? getenv("SPLITFRAME_ORDER") : "raw-m2v-raw";
        const bool m2vFirst = !strcmp(order, "m2v-raw"), rawOnly = !strcmp(order, "raw");
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVDEC_WRITETHROUGH", "1", 0);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        if (!m2v) {
            fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        NSString *why = nil;
        id<MTLDevice> dev = raw_metal_device_open(argv[1], 0, m2v, &why);
        if (!dev) {
            fprintf(stderr, "NO RAW DEVICE: %s\n", why.UTF8String);
            return 1;
        }
        const unsigned bytes = 16 * 1024;
        auto shared = [&](id<MTLDevice> d) {
            id<MTLBuffer> b = [d newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            memset(b.contents, 0, bytes);
            return b;
        };
        std::vector<Chain> chains(threads);
        for (unsigned t = 0; t < threads; t++) {
            for (unsigned i = 0; i <= count; i++)
                chains[t].links.push_back(shared(dev));
            for (unsigned i = 0; i < count; i++) {
                chains[t].a.push_back(shared(dev));
                chains[t].b.push_back(shared(dev));
            }
            for (unsigned i = 0; i < bytes; i++)
                ((uint8_t *)chains[t].links[0].contents)[i] = uint8_t(i * 7 + 3 + t * 31);
        }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:64
                                                                                     height:64
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        std::atomic<unsigned> scheduledRan{0}, completedRan{0}, errors{0}, misordered{0};
        // warm-up, waited for: the raw channel's first submission (its context) and m2v's first-use work stay out of the window
        {
            id<MTLBuffer> scratch = shared(dev), own = shared(m2v);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:chains[0].links[0] sourceOffset:0 toBuffer:scratch destinationOffset:0 size:bytes];
            [e endEncoding];
            e = [cb blitCommandEncoder];
            [e copyFromBuffer:scratch sourceOffset:0 toBuffer:own destinationOffset:0 size:bytes];
            [e endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        char line[2048];
        raw_metal_rates(line, sizeof line);
        printf("BEGIN %.3f\n", nowMs());
        const double began = nowMs();
        std::vector<std::thread> committers;
        std::vector<id<MTLCommandBuffer>> lasts(threads);
        for (unsigned t = 0; t < threads; t++)
            committers.emplace_back([&, t] {
                Chain &c = chains[t];
                id<MTLBuffer> own = shared(m2v);
                for (unsigned k = 0; k < count; k++) {
                    @autoreleasepool {
                        id<MTLCommandBuffer> cb = [q commandBuffer];
                        id<MTLBlitCommandEncoder> e;
                        id<MTLBuffer> into = c.a[k];
                        if (!m2vFirst) {
                            e = [cb blitCommandEncoder];
                            [e copyFromBuffer:c.links[k] sourceOffset:0 toBuffer:into destinationOffset:0 size:bytes];
                            [e endEncoding];
                        } else {
                            into = c.links[k];
                        }
                        e = [cb blitCommandEncoder];
                        if (rawOnly) {
                            [e copyFromBuffer:into sourceOffset:0 toBuffer:c.b[k] destinationOffset:0 size:bytes];
                        } else {
                            [e copyFromBuffer:into sourceOffset:0 toBuffer:own destinationOffset:0 size:bytes];
                            [e copyFromBuffer:own sourceOffset:0 toBuffer:c.b[k] destinationOffset:0 size:bytes];
                        }
                        [e endEncoding];
                        e = [cb blitCommandEncoder];
                        [e copyFromBuffer:c.b[k] sourceOffset:0 toBuffer:c.links[k + 1] destinationOffset:0 size:bytes];
                        [e endEncoding];
                        id<MTLTexture> seen = [dev newTextureWithDescriptor:td];
                        auto sched = std::make_shared<std::atomic<bool>>(false);
                        [cb addScheduledHandler:^(id<MTLCommandBuffer>) {
                            (void)seen.width;
                            sched->store(true);
                            scheduledRan++;
                        }];
                        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                            if (!sched->load())
                                misordered++;
                            id<MTLTexture> view = [seen newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
                            (void)view.pixelFormat;
                            if (done.error)
                                errors++;
                            completedRan++;
                        }];
                        [cb commit];
                        [cb waitUntilScheduled];
                        lasts[t] = cb;
                    }
                }
            });
        for (std::thread &t : committers)
            t.join();
        const double committed = nowMs() - began;
        for (id<MTLCommandBuffer> cb : lasts)
            [cb waitUntilCompleted];
        const double done = nowMs() - began;
        while (completedRan.load() < threads * count && nowMs() - began < 20000)
            usleep(1000);
        usleep(200000);  // scheduled handlers still queued run meanwhile; only lost ones stay uncounted
        bool intact = true;
        for (unsigned t = 0; t < threads; t++)
            intact &= !memcmp(chains[t].links[0].contents, chains[t].links[count].contents, bytes);
        printf("commits %u on %u threads (%s) in %.3f ms, all complete after %.3f ms; scheduled %u completed %u "
               "(before their "
               "scheduled %u) errors %u; chains %s\n",
               threads * count, threads, order, committed, done, scheduledRan.load(), completedRan.load(),
               misordered.load(), errors.load(), intact ? "intact" : "BROKEN");
        if (raw_metal_rates(line, sizeof line))
            printf("rates: %s\n", line);
        const bool handlers = scheduledRan.load() == threads * count && completedRan.load() == threads * count;
        return intact && handlers && !errors.load() && !misordered.load() ? 0 : 1;
    }
}
