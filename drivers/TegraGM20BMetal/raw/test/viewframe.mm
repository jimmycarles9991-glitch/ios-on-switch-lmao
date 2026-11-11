// SPDX-License-Identifier: MIT
// viewframe: kernels m2v binds a view of another format for (hw_test_app/shaders/HWTest.metal view_*), over the mock
// GPU, through m2v + NVK (nvk) and through the raw facade with its kernels made at their first dispatch (facade). A
// half-float texture a kernel writes is m2v's unsigned integer twin (R16_UINT, R16G16B16A16_UINT): the raw path binds
// a header of that format over the texture's memory. The facade run checks every command buffer took the raw path;
// test/viewframe.sh compares the two decodes (each slot's header, all eight words).
//   viewframe nvk <nvk dylib> <cache dir> <metallib>
//   viewframe facade <nvk dylib> <cache dir> <metallib> <pack.bin>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "../runtime/RawMetal.h"

namespace {

constexpr NSUInteger kWidth = 32, kHeight = 16;

struct Slot {
    MTLPixelFormat format;
    MTLTextureUsage usage;
};

struct Case {
    const char *kernel;
    std::vector<Slot> slots;
    // its textures an MTLHeap's, as MPS's intermediate images are, on the facade: m2v reads a heap's textures through its
    // heap variant, whose set 0 holds none, so the nvk run makes them outside one (NVK's set of the kernel's own slots)
    bool heap = false;
};

constexpr MTLTextureUsage kRead = MTLTextureUsageShaderRead, kWrite = MTLTextureUsageShaderWrite;

const Case kCases[] = {
    {"view_half_write",
     {{MTLPixelFormatRGBA16Float, kRead}, {MTLPixelFormatRGBA16Float, kWrite}, {MTLPixelFormatR16Float, kWrite}}},
    {"view_bgra_write", {{MTLPixelFormatBGRA8Unorm, kWrite}}},
    {"view_half_read_write", {{MTLPixelFormatRGBA16Float, kRead | kWrite}}},
    {"view_half_write",
     {{MTLPixelFormatRGBA16Float, kRead}, {MTLPixelFormatRGBA16Float, kWrite}, {MTLPixelFormatR16Float, kWrite}},
     true},
    // controls: a format m2v writes as itself, and the half texture read alone
    {"view_bgra_write", {{MTLPixelFormatRGBA8Unorm, kWrite}}},
    {"read_float_texture", {{MTLPixelFormatRGBA16Float, kRead}}},
};

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

NSString *path(id<MTLCommandBuffer> cb)
{
    const SEL sel = NSSelectorFromString(@"switchRawPath");
    return [(id)cb respondsToSelector:sel] ? ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel) : @"device";
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        const bool facade = argc >= 6 && !strcmp(argv[1], "facade");
        if (!facade && !(argc >= 5 && !strcmp(argv[1], "nvk"))) {
            fprintf(stderr, "usage: viewframe nvk|facade <nvk dylib> <cache dir> <metallib> [<pack.bin>]\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[2], argv[3], 0, missing);
        if (dev && facade) {
            NSString *why = nil;
            dev = raw_metal_device_open(argv[5], 0, dev, &why);
            if (dev)
                raw_metal_complete_states(nullptr);
            else
                fprintf(stderr, "NO FACADE %s\n", why.UTF8String);
        }
        if (!dev) {
            fprintf(stderr, "NO DEVICE %s\n", nmb_last_error());
            return 1;
        }
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4])] error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> out = [dev newBufferWithLength:kWidth * kHeight * 16 options:MTLResourceStorageModeShared];
        int failed = 0;
        for (const Case &c : kCases) {
            id<MTLFunction> f = [lib newFunctionWithName:@(c.kernel)];
            id<MTLComputePipelineState> ps = f ? [dev newComputePipelineStateWithFunction:f error:&e] : nil;
            if (!ps) {
                fprintf(stderr, "NO STATE %s: %s\n", c.kernel, e.localizedDescription.UTF8String ?: "no function");
                return 1;
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:ps];
            [enc setBuffer:out offset:0 atIndex:0];
            NSMutableArray<MTLTextureDescriptor *> *descriptors = [NSMutableArray array];
            NSUInteger heapBytes = 0;
            for (const Slot &slot : c.slots) {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:slot.format
                                                                                             width:kWidth
                                                                                            height:kHeight
                                                                                         mipmapped:NO];
                d.usage = slot.usage;
                d.storageMode = MTLStorageModePrivate;
                const MTLSizeAndAlign size = [dev heapTextureSizeAndAlignWithDescriptor:d];
                heapBytes += (size.size + size.align - 1) / size.align * size.align + size.align;
                [descriptors addObject:d];
            }
            id<MTLHeap> heap = nil;
            if (c.heap && facade) {
                MTLHeapDescriptor *h = [MTLHeapDescriptor new];
                h.size = heapBytes;
                h.storageMode = MTLStorageModePrivate;
                heap = [dev newHeapWithDescriptor:h];
            }
            for (NSUInteger s = 0; s < descriptors.count; s++) {
                id<MTLTexture> t = heap ? [heap newTextureWithDescriptor:descriptors[s]]
                                        : [dev newTextureWithDescriptor:descriptors[s]];
                if (!t) {
                    fprintf(stderr, "NO TEXTURE %s slot %lu\n", c.kernel, (unsigned long)s);
                    return 1;
                }
                [enc setTexture:t atIndex:s];
            }
            [enc dispatchThreadgroups:MTLSizeMake(kWidth / 8, kHeight / 8, 1)
                threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const bool ok =
                cb.status == MTLCommandBufferStatusCompleted && (!facade || [path(cb) isEqualToString:@"raw"]);
            printf("%s %s %u%s: %s\n", ok ? "OK  " : "FAIL", c.kernel, uint32_t(c.slots[0].format),
                   c.heap ? " heap" : "", path(cb).UTF8String);
            failed += !ok;
        }
        if (facade) {
            char rates[2048];
            raw_metal_rates(rates, sizeof rates);
            printf("facade rates: %s\n", rates);
        }
        printf("viewframe %s: %d case(s) failed\n", argv[1], failed);
        return failed ? 1 : 0;
    }
}
