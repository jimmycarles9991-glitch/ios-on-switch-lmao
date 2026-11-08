// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): the two drivers' order on the GPU. Command buffers alternate between the raw path (blit copies
// that chain a pattern from buffer to buffer) and m2v (a mipmap generation beside a compute encoder, which the raw path hands to
// m2v whole), committed back to back with no wait; only the last is waited for. Over the mock's asynchronous GPU (mock_iokit.c MOCK_GPU_US: each
// submission takes that long, in kick order per channel, after the acquires the kernel's Order queued for it), the commits
// must all return long before the GPU is done (no CPU wait for GPU work), the mock's log must show every submission start
// after the one committed before it on the other driver ended (ordercheck.py), the chain must arrive intact, and the rates line
// must count the switches with none ordered on the CPU. MOCK_NO_ORDER=1 (a kernel without Order): the same order and content,
// by CPU waits, which the rates line counts. ORDERFRAME_PACE_US: that long between pairs, as frames come, so the GPU can kill
// a channel (mock_iokit.c MOCK_KILL) while later pairs are still being committed: the raw path makes its channel again and
// keeps GPU order (ordercheck.py's first raw channel argument).
// usage: orderframe <pack> <cache> <nvk host dylib> [pairs]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
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

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: orderframe <pack> <cache> <nvk dylib> [pairs]\n");
            return 2;
        }
        const unsigned pairs = argc > 4 ? unsigned(atoi(argv[4])) : 6;
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
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const unsigned bytes = 64 * 1024;
        std::vector<id<MTLBuffer>> chain;
        for (unsigned i = 0; i <= pairs; i++) {
            chain.push_back([dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]);
            memset(chain.back().contents, 0, bytes);
        }
        for (unsigned i = 0; i < bytes; i++)
            ((uint8_t *)chain[0].contents)[i] = uint8_t(i * 7 + 3);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:64
                                                                                    height:64
                                                                                 mipmapped:YES];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> mip = [dev newTextureWithDescriptor:d];
        // warm-up: one of each, waited for, so the measured run has no first-use work in it
        for (int warm = 0; warm < 2; warm++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            if (warm)
                [b generateMipmapsForTexture:mip];
            else
                [b copyFromBuffer:chain[0] sourceOffset:0 toBuffer:chain[pairs] destinationOffset:0 size:16];
            [b endEncoding];
            if (warm)
                [[cb computeCommandEncoder] endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        memset(chain[pairs].contents, 0, bytes);
        char line[1024];
        raw_metal_rates(line, sizeof line);  // the window starts here
        printf("BEGIN %.3f\n", nowMs());
        const double began = nowMs();
        const useconds_t pace = getenv("ORDERFRAME_PACE_US") ? useconds_t(atoi(getenv("ORDERFRAME_PACE_US"))) : 0;
        id<MTLCommandBuffer> last = nil;
        for (unsigned k = 0; k < pairs; k++) {
            if (k && pace)
                usleep(pace);
            id<MTLCommandBuffer> r = [q commandBuffer];  // raw: chain[k] -> chain[k + 1]
            id<MTLBlitCommandEncoder> b = [r blitCommandEncoder];
            [b copyFromBuffer:chain[k] sourceOffset:0 toBuffer:chain[k + 1] destinationOffset:0 size:bytes];
            [b endEncoding];
            [r commit];
            id<MTLCommandBuffer> m = [q commandBuffer];  // m2v: the raw path hands compute to it
            b = [m blitCommandEncoder];
            [b generateMipmapsForTexture:mip];
            [b endEncoding];
            [[m computeCommandEncoder] endEncoding];
            [m commit];
            last = m;
        }
        const double committed = nowMs() - began;
        [last waitUntilCompleted];
        const double done = nowMs() - began;
        const bool intact = !memcmp(chain[0].contents, chain[pairs].contents, bytes);
        printf("commits %u in %.3f ms, all complete after %.3f ms; chain %s\n", 2 * pairs, committed, done,
               intact ? "intact" : "BROKEN");
        if (raw_metal_rates(line, sizeof line))
            printf("rates: %s\n", line);
        return intact ? 0 : 1;
    }
}
