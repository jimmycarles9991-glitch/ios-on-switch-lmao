// SPDX-License-Identifier: MIT
// computeframe: one Metal kernel dispatched through the shipped frontend + m2v + NVK and through the raw path's compute encoder
// (runtime/raw_compute.h), over the mock GPU; test/computeeq.sh compares the two decodes (pack/eqcheck.py's DISPATCH records).
//   computeframe nvk <nvk dylib> <cache dir> <metallib> <kernel> <threads x,y,z> <groups x,y,z>
//     with NVDEC_KERNELS set, its log's K lines are the kernel's record (pack/nvdec_compute.cpp)
//   computeframe raw <pack.bin> <nvk log> <programs.bin> <threads x,y,z> <groups x,y,z>
//   computeframe pack <pack.bin> - <key> <threads x,y,z> <groups x,y,z>: the pack's kernel of that raw_compute_key.h key
// Both bind every buffer slot 0..kSlots-1 to the same tagged bytes (pack/probe.mm's scheme).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "computeframe.h"
#include "bridge/NativeMetalObjects.h"
#include "../runtime/raw_compute.h"
#include "../runtime/raw_compute_key.h"
#ifdef RAW_FACADE  // computefacade: the nvk mode's dispatch through the raw path's Metal facade (runtime/RawMetal.mm)
#include "../runtime/RawMetal.h"
#endif

extern "C" void pushcount_report(const char *what);

namespace {

using namespace computeframe;

// FRAME_BLIT_FIRST: a fill before the compute encoder, in the same command buffer (over the facade, a refused compute
// encoder sends only its own segment to m2v: the rates line's sp=)
void blitFirst(id<MTLDevice> dev, id<MTLCommandBuffer> cb)
{
    id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:b range:NSMakeRange(0, 4096) value:0x5a];
    [blit endEncoding];
}

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

int runNvk(char **argv)
{
    uint32_t threads[3], groups[3];
    if (!size3(argv[6], threads) || !size3(argv[7], groups))
        return 2;
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    id<MTLDevice> dev = native_bridge_metal_open(argv[2], argv[3], 0, missing);
    if (!dev) {
        fprintf(stderr, "NO DEVICE %s\n", nmb_last_error());
        return 1;
    }
#ifdef RAW_FACADE
    // the raw facade over the m2v device, as the render server opens it, with the pack FRAME_PACK names
    NSString *why = nil;
    dev = raw_metal_device_open(getenv("FRAME_PACK"), 0, dev, &why);
    if (!dev) {
        fprintf(stderr, "NO FACADE %s\n", why.UTF8String);
        return 1;
    }
    // FRAME_COMPLETE=<dir>|memory: a kernel the pack lacks is made at its first dispatch (RawComplete.h), kept in <dir>
    if (const char *complete = getenv("FRAME_COMPLETE"))
        raw_metal_complete_states(strcmp(complete, "memory") ? complete : nullptr);
#endif
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4])] error:&e];
    id<MTLFunction> f = [lib newFunctionWithName:@(argv[5])];
    id<MTLComputePipelineState> ps = f ? [dev newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!ps) {
        fprintf(stderr, "NO STATE %s\n", e.localizedDescription.UTF8String ?: "no function");
        return 1;
    }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (getenv("FRAME_BLIT_FIRST"))
        blitFirst(dev, cb);
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    for (uint32_t s = 0; s < kSlots; s++)
        [enc setBytes:tag(s).data() length:kTagBytes atIndex:s];
    if (const uint32_t big = bigBuffer()) {
        id<MTLBuffer> b = [dev newBufferWithLength:big options:MTLResourceStorageModeShared];
        fillBig(static_cast<uint8_t *>(b.contents), big);
        [enc setBuffer:b offset:0 atIndex:kBigSlot];
    }
    if (uint32_t slot, offset; off16(slot, offset)) {
        id<MTLBuffer> b = [dev newBufferWithLength:offset + kTagBytes options:MTLResourceStorageModeShared];
        memset(b.contents, 0, offset);
        memcpy(static_cast<uint8_t *>(b.contents) + offset, tag(slot).data(), kTagBytes);
        [enc setBuffer:b offset:offset atIndex:slot];
    }
    if (const char *length = getenv("FRAME_THREADGROUP_MEMORY"))
        [enc setThreadgroupMemoryLength:strtoul(length, nullptr, 0) atIndex:0];
    [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    for (uint32_t t = 0; t < boundTextures(); t++) {
        const char *format = getenv("FRAME_TEXTURE_FORMAT");
        MTLTextureDescriptor *d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:format ? MTLPixelFormat(atoi(format)) : MTLPixelFormatRGBA8Unorm
                                         width:16 + t
                                        height:8 + t
                                     mipmapped:NO];
        d.usage = getenv("FRAME_TEXTURE_READ_ONLY") ? MTLTextureUsageShaderRead
                                                    : MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        id<MTLTexture> x = [dev newTextureWithDescriptor:d];
        [x replaceRegion:MTLRegionMake2D(0, 0, 16 + t, 8 + t)
             mipmapLevel:0
               withBytes:texels(t).data()
             bytesPerRow:4 * (16 + t)];
        [enc setTexture:x atIndex:t];
        if (getenv("FRAME_NO_SAMPLERS"))
            continue;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.lodMaxClamp = 1 + t / 2.0f;
        [enc setSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:t];
    }
    for (uint32_t n = 0; n < dispatches(); n++)
        [enc dispatchThreadgroups:MTLSizeMake(groups[0], groups[1], groups[2])
            threadsPerThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    pushcount_report("computeframe nvk");
#ifdef RAW_FACADE
    char rates[2048];
    raw_metal_rates(rates, sizeof rates);
    fprintf(stderr, "facade rates: %s\n", rates);
#endif
    return cb.status == MTLCommandBufferStatusCompleted ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc >= 8 && (!strcmp(argv[1], "nvk") || !strcmp(argv[1], "facade")))
            return runNvk(argv);
        if (argc >= 7 && (!strcmp(argv[1], "raw") || !strcmp(argv[1], "pack")))
            return computeframeRaw(argv);
        fprintf(stderr, "usage: computeframe nvk <nvk> <cache> <metallib> <kernel> <threads x,y,z> <groups x,y,z>\n"
                        "       computeframe raw <pack.bin> <nvk log> <programs.bin> <threads x,y,z> <groups x,y,z>\n"
                        "       computeframe pack <pack.bin> - <key> <threads x,y,z> <groups x,y,z>\n");
        return 2;
    }
}
