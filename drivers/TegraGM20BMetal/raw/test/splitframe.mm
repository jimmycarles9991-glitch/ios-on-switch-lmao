// SPDX-License-Identifier: MIT
// Command buffers the facade submits in segments (planSegments): each one copies a pattern down a chain of raw buffers through a
// blit encoder that only m2v takes (a copy through an m2v buffer of its own, call.bufcopy), between raw copies, so its data
// crosses raw -> m2v -> raw inside one command buffer (SPLITFRAME_ORDER: the encoders' drivers, as m2v-raw or m2v-raw-m2v; raw:
// three raw ones). Committed
// back to back from SPLITFRAME_THREADS threads (Core Animation's render thread and the others that commit), each waiting until
// scheduled as Core Animation's render thread does, with scheduled and completed handlers that ask the raw objects for their m2v
// twins (Core Animation's handlers touch the textures of the frame). Over the mock's GPU (MOCK_GPU_US; 0 races the completion
// thread against the submitting one) every chain must arrive intact and every handler run, each command buffer's scheduled
// ones before its completed ones; ordercheck.py reads MOCK_GPU_LOG for the order on the GPU.
// SPLITFRAME_EVENTS=1: each command buffer of a thread waits for its thread's shared event before its first encoder (command
// buffer k: value k + 1) and signals the next value after its last, as Core Animation's per-surface events do. The first waits
// for the host, which writes the chain's first link only then and signals: a first segment run before the signal copies the
// old bytes down the chain (BROKEN). Each signal must come with its command buffer's last link written, in order, and the event
// end at the last value; then a wait with a 50 ms timeout for a value nothing signals goes on after the timeout.
// usage: NVK_COPIES=ce splitframe <pack> <cache> <nvk host dylib> [command buffers per thread] (NVK_COPIES=ce: m2v's copies on
// the copy engine, which the mock runs; its shader copies it does not, and every chain through m2v would read as broken)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
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
        // each encoder of a command buffer, by driver: a raw copy, or one through an m2v buffer (true)
        std::vector<bool> stages;
        if (!strcmp(order, "raw"))
            stages.assign(3, false);
        else
            for (NSString *s in [@(order) componentsSeparatedByString:@"-"])
                stages.push_back([s isEqualToString:@"m2v"]);
        if (stages.size() > 3) {
            fprintf(stderr, "splitframe: at most three encoders (%s)\n", order);
            return 2;
        }
        const bool events = getenv("SPLITFRAME_EVENTS") != nullptr;
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
            if (!events)
                for (unsigned i = 0; i < bytes; i++)
                    ((uint8_t *)chains[t].links[0].contents)[i] = uint8_t(i * 7 + 3 + t * 31);
        }
        std::vector<id<MTLSharedEvent>> signals(threads);
        std::atomic<unsigned> notified{0}, early{0};
        MTLSharedEventListener *listener =
            [[MTLSharedEventListener alloc] initWithDispatchQueue:dispatch_queue_create("splitframe", nullptr)];
        for (unsigned t = 0; t < threads && events; t++) {
            signals[t] = [dev newSharedEvent];
            std::atomic<unsigned> *seen = &notified, *before = &early;
            for (unsigned k = 0; k < count; k++) {
                id<MTLBuffer> first = chains[t].links[0], last = chains[t].links[k + 1];
                [signals[t] notifyListener:listener
                                   atValue:k + 2
                                     block:^(id<MTLSharedEvent>, uint64_t) {
                                         *before += memcmp(first.contents, last.contents, bytes) != 0;
                                         (*seen)++;
                                     }];
            }
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
        if (events)
            committers.emplace_back([&] {
                usleep(30000);
                for (unsigned t = 0; t < threads; t++) {
                    for (unsigned i = 0; i < bytes; i++)
                        ((uint8_t *)chains[t].links[0].contents)[i] = uint8_t(i * 7 + 3 + t * 31);
                    signals[t].signaledValue = 1;
                }
            });
        for (unsigned t = 0; t < threads; t++)
            committers.emplace_back([&, t] {
                Chain &c = chains[t];
                id<MTLBuffer> own = shared(m2v);
                for (unsigned k = 0; k < count; k++) {
                    @autoreleasepool {
                        id<MTLCommandBuffer> cb = [q commandBuffer];
                        if (events)
                            [cb encodeWaitForEvent:signals[t] value:k + 1];
                        const id<MTLBuffer> between[2] = {c.a[k], c.b[k]};
                        for (size_t i = 0; i < stages.size(); i++) {
                            id<MTLBuffer> from = i ? between[i - 1] : c.links[k],
                                          to = i + 1 < stages.size() ? between[i] : c.links[k + 1];
                            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
                            if (stages[i]) {
                                [e copyFromBuffer:from sourceOffset:0 toBuffer:own destinationOffset:0 size:bytes];
                                [e copyFromBuffer:own sourceOffset:0 toBuffer:to destinationOffset:0 size:bytes];
                            } else {
                                [e copyFromBuffer:from sourceOffset:0 toBuffer:to destinationOffset:0 size:bytes];
                            }
                            [e endEncoding];
                        }
                        if (events)
                            [cb encodeSignalEvent:signals[t] value:k + 2];
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
        bool intact = true, signalled = true;
        for (unsigned t = 0; t < threads; t++) {
            intact &= !memcmp(chains[t].links[0].contents, chains[t].links[count].contents, bytes);
            signalled &= !events || signals[t].signaledValue == count + 1;
        }
        if (events)
            printf("events: %u signals notified of %u, %u before their command buffer's last link, final values %s\n",
                   notified.load(), threads * count, early.load(), signalled ? "right" : "WRONG");
        printf("commits %u on %u threads (%s) in %.3f ms, all complete after %.3f ms; scheduled %u completed %u "
               "(before their "
               "scheduled %u) errors %u; chains %s\n",
               threads * count, threads, order, committed, done, scheduledRan.load(), completedRan.load(),
               misordered.load(), errors.load(), intact ? "intact" : "BROKEN");
        if (raw_metal_rates(line, sizeof line))
            printf("rates: %s\n", line);
        const bool handlers = scheduledRan.load() == threads * count && completedRan.load() == threads * count;
        double timedMs = 0;
        if (events) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            ((void (*)(id, SEL, id, uint64_t, uint32_t))objc_msgSend)(
                cb, sel_registerName("encodeWaitForEvent:value:timeout:"), [dev newSharedEvent], 1, 50);
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:chains[0].links[0] sourceOffset:0 toBuffer:chains[0].a[0] destinationOffset:0 size:bytes];
            [e endEncoding];
            const double from = nowMs();
            [cb commit];
            [cb waitUntilCompleted];
            timedMs = nowMs() - from;
            printf("a wait with a 50 ms timeout for a value never signalled: completed after %.1f ms\n", timedMs);
        }
        const bool ordered = !events || (signalled && !early.load() && notified.load() == threads * count &&
                                         timedMs >= 50 && timedMs < 1000);
        return intact && handlers && ordered && !errors.load() && !misordered.load() ? 0 : 1;
    }
}
