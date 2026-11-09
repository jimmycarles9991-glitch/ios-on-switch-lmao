// SPDX-License-Identifier: MIT
// The raw device's MTLHeaps as RenderBox uses them (ResourceHeap: heap textures, makeAliasable, usedSize, the heap made
// volatile when recycled), through the facade over the mock GPU with no m2v device: a private heap holds its size, places
// block-linear textures until it answers nil, takes an aliasable texture's room again, gives its pages back when made Empty
// with nothing in flight, and has them again for a command buffer that draws into one of its textures.
// usage: heapframe <pack.bin>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../runtime/RawMetal.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: heapframe <pack.bin>\n");
        return 2;
    }
    int failures = 0;
    auto expect = [&](bool ok, const char *what) {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    @autoreleasepool {
        NSString *why = nil;
        id<MTLDevice> dev = raw_metal_device_open(argv[1], 0, nil, &why);
        if (!dev) {
            std::fprintf(stderr, "open: %s\n", why.UTF8String);
            return 1;
        }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:512
                                                                                     height:512
                                                                                  mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        const MTLSizeAndAlign need = [dev heapTextureSizeAndAlignWithDescriptor:td];
        expect(need.size == 1u << 20 && need.align % 16384 == 0, "heapTextureSizeAndAlign is the raw layout's");
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.size = 4 * need.size;
        hd.storageMode = MTLStorageModePrivate;
        hd.type = MTLHeapTypeAutomatic;
        id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
        NSMutableArray<id<MTLTexture>> *textures = [NSMutableArray array];
        for (int i = 0; i < 4; i++)
            if (id<MTLTexture> t = [heap newTextureWithDescriptor:td])
                [textures addObject:t];
        expect(textures.count == 4 && textures[2].heap == heap, "four textures fill the heap, each its heap's");
        expect([heap newTextureWithDescriptor:td] == nil, "a full heap answers nil");
        expect(heap.usedSize == hd.size && [heap maxAvailableSizeWithAlignment:need.align] == 0,
               "usedSize counts its textures");
        const NSUInteger offset = textures[1].heapOffset;
        [textures[1] makeAliasable];
        id<MTLTexture> again = [heap newTextureWithDescriptor:td];
        expect(textures[1].isAliasable && again && again.heapOffset == offset,
               "an aliasable texture's room is placed again");

        expect([heap setPurgeableState:MTLPurgeableStateEmpty] == MTLPurgeableStateNonVolatile,
               "Empty answers the state before");
        expect([heap setPurgeableState:MTLPurgeableStateKeepCurrent] == MTLPurgeableStateEmpty &&
                   [again setPurgeableState:MTLPurgeableStateKeepCurrent] == MTLPurgeableStateEmpty,
               "nothing in flight: its pages went, and its textures say so");
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = again;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        expect(cb.status == MTLCommandBufferStatusCompleted && cb.error == nil,
               "a pass into a purged heap's texture runs");
        expect([heap setPurgeableState:MTLPurgeableStateKeepCurrent] != MTLPurgeableStateEmpty,
               "the commit gave the heap its pages again");
        expect([heap setPurgeableState:MTLPurgeableStateNonVolatile] == MTLPurgeableStateVolatile,
               "NonVolatile answers Volatile once restored");
    }
    std::printf("%s\n", failures ? "heapframe FAILED" : "heapframe ok");
    return failures ? 1 : 0;
}
