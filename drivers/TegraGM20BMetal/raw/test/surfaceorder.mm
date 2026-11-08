// SPDX-License-Identifier: MIT
// An IOSurface one process's raw path writes and another process reads is read after the write on the GPU, with no CPU wait:
// over the mock's asynchronous GPU (mock_iokit.c: each connection another task's, ExecSurfaces ordering as the kext), the
// reader's submission starts after the writer's ended, and queued an acquire for it.
//   facade: an app's raw facade (a client's device on NVK's connection) clears the surface through Metal; the render server's raw
//           device (a connection of its own) copies it out
//   m2v:    an app's raw device (a connection of its own) clears the surface; the render server's m2v device (NVK's connection)
//           copies it out
// MOCK_IGNORE_ACQUIRES=1, the negative control: the reader overlaps the writer, and the check fails.
// usage: MOCK_GPU_US=30000 surfaceorder <pack> <cache> <nvk host dylib> facade|m2v
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <string>
#include <vector>
#include "../runtime/RawMetal.h"
#include "../runtime/raw_gpu.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

extern "C" unsigned mock_iokit_runs(uint64_t (*out)[6], unsigned most);

namespace {
constexpr uint32_t kSide = 256, kBgra8 = 80;

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

IOSurfaceRef newSurface()
{
    NSDictionary *p = @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(kSide * 4),
        (id)kIOSurfacePixelFormat : @(0x42475241u),  // 'BGRA'
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

MTLTextureDescriptor *surfaceDescriptor(MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.usage = usage;
    return d;
}

// One process's raw device: the surface's texture, cleared (writer) or copied out of into a texture of its own (reader)
struct RawSide {
    raw::Device dev;
    raw::Texture surface, copy;

    bool open(const char *pack, raw::Device::Role role, IOSurfaceRef s, bool writes)
    {
        std::string err;
        if (!dev.open(pack, 0, err, role)) {
            fprintf(stderr, "raw device: %s\n", err.c_str());
            return false;
        }
        const uint32_t id = IOSurfaceGetID(s);
        if (!dev.createSurfaceTexture(id, IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s, 0, kSide, kSide,
                                      uint32_t(IOSurfaceGetBytesPerRow(s)), kBgra8, surface, writes))
            return false;
        return (!writes || dev.surfaceGpuWrites(id)) && (writes || dev.createTexture(kSide, kSide, kBgra8, copy));
    }

    bool submit(bool writes, uint32_t &seq)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return false;
        if (writes) {
            raw::PassDesc pass;
            pass.count = 1;
            pass.color[0].texture = &surface;
            pass.color[0].load = raw::ColorAttachment::Clear;
            pass.color[0].clear[0] = pass.color[0].clear[3] = 1;
            raw::RenderEncoder *re = cb->render(pass);
            if (!re)
                return false;
            re->end();
        } else {
            raw::BlitEncoder *be = cb->blit();
            be->copyTexels(surface, 0, 0, copy, 0, 0, kSide, kSide);
            be->end();
        }
        cb->use(surface.import, writes);
        return cb->commit(seq);
    }
};

struct Run {
    uint64_t ch, seqno, kick, start, end, acquires;
};

std::vector<Run> runsKickedIn(uint64_t from, uint64_t to)
{
    static uint64_t raw[8192][6];
    std::vector<Run> out;
    for (unsigned i = 0, n = mock_iokit_runs(raw, 8192); i < n; i++)
        if (raw[i][2] >= from && raw[i][2] <= to)
            out.push_back({raw[i][0], raw[i][1], raw[i][2], raw[i][3], raw[i][4], raw[i][5]});
    return out;
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 5 || !getenv("MOCK_GPU_US")) {
            fprintf(stderr, "usage: MOCK_GPU_US=<us> surfaceorder <pack> <cache> <nvk dylib> facade|m2v\n");
            return 2;
        }
        const bool facade = !strcmp(argv[4], "facade");
        setenv("MOCK_TASK_WINDOWS", "1", 1);  // the two processes' GPU VAs apart, as two tasks' are
        IOSurfaceRef s = newSurface();
        // the process that opens its own connection first: NVK not loaded yet (raw_kernel.cpp Kernel::open)
        RawSide own;
        if (!own.open(argv[1], facade ? raw::Device::Role::RenderServer : raw::Device::Role::Client, s, !facade))
            return 1;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        if (!m2v) {
            fprintf(stderr, "no m2v device: %s\n", nmb_last_error());
            return 1;
        }
        NSString *why = nil;
        id<MTLDevice> app = facade ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (facade && !app) {
            fprintf(stderr, "no raw facade: %s\n", why.UTF8String);
            return 1;
        }
        id<MTLDevice> nvkSide = facade ? app : m2v;
        id<MTLTexture> t = [nvkSide
            newTextureWithDescriptor:surfaceDescriptor(facade ? MTLTextureUsageRenderTarget : MTLTextureUsageShaderRead)
                           iosurface:s
                               plane:0];
        id<MTLBuffer> out = [m2v newBufferWithLength:kSide * kSide * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [nvkSide newCommandQueue];
        auto metal = [&]() -> id<MTLCommandBuffer> {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            if (facade) {
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = t;
                p.colorAttachments[0].loadAction = MTLLoadActionClear;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                p.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
                [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
            } else {
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b copyFromTexture:t
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:MTLSizeMake(kSide, kSide, 1)
                                    toBuffer:out
                           destinationOffset:0
                      destinationBytesPerRow:kSide * 4
                    destinationBytesPerImage:kSide * kSide * 4];
                [b endEncoding];
            }
            [cb commit];
            return cb;
        };
        // each side's first submissions (a client's channel and context, NVK's queue) before the ones measured
        uint32_t seq = 0;
        if (!own.submit(!facade, seq) || !own.dev.wait(seq))
            return 1;
        [metal() waitUntilCompleted];
        // the writer, then at once the reader in the other process
        const uint64_t w0 = nowUs();
        id<MTLCommandBuffer> m = nil;
        bool ok = true;
        if (facade)
            m = metal();
        else
            ok = own.submit(true, seq);
        const uint64_t w1 = nowUs();
        if (facade)
            ok = own.submit(false, seq);
        else
            m = metal();
        const uint64_t r1 = nowUs();
        [m waitUntilCompleted];
        ok = ok && own.dev.wait(seq) && m.status == MTLCommandBufferStatusCompleted;
        const std::vector<Run> writes = runsKickedIn(w0, w1), reads = runsKickedIn(w1, r1);
        if (!ok || writes.empty() || reads.empty()) {
            fprintf(stderr, "FAIL: no runs to compare (ok %d, %zu writer runs, %zu reader runs)\n", ok, writes.size(),
                    reads.size());
            return 1;
        }
        uint64_t writeEnd = 0, readStart = ~0ull, acquires = 0;
        for (const Run &r : writes)
            writeEnd = std::max(writeEnd, r.end);
        for (const Run &r : reads) {
            readStart = std::min(readStart, r.start);
            acquires += r.acquires;
        }
        const bool ordered = readStart >= writeEnd && acquires;
        printf(
            "%s: writer ch %llu ends at %llu us, reader ch %llu starts at %llu us (%+lld), %llu acquires queued: %s\n",
            argv[4], (unsigned long long)writes.back().ch, (unsigned long long)(writeEnd - w0),
            (unsigned long long)reads.front().ch, (unsigned long long)(readStart - w0),
            (long long)(readStart - writeEnd), (unsigned long long)acquires, ordered ? "ordered" : "OVERLAP");
        printf("%s: surfaceorder %s\n", ordered ? "PASS" : "FAIL", argv[4]);
        return ordered ? 0 : 1;
    }
}
