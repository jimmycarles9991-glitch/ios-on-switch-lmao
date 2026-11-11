// SPDX-License-Identifier: MIT
// A purged heap must be resident before either resource can create its fallback twin.
// usage: purgetwin <pack> <cache> <nvk dylib>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../runtime/RawMetalInternal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static void missing(const char *, const char *)
{}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (!dev)
            return 1;
        int failed = 0;
        for (unsigned resource = 0; resource < 3; ++resource) {
            const MTLStorageMode mode = resource == 0 ? MTLStorageModePrivate : MTLStorageModeShared;
            MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
            hd.size = 2 << 20;
            hd.storageMode = mode;
            id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:140
                                                                                        height:143
                                                                                     mipmapped:NO];
            d.storageMode = mode;
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            RMTexture *t = resource < 2 ? (RMTexture *)[heap newTextureWithDescriptor:d] : nil;
            RMBuffer *b =
                resource == 2 ? (RMBuffer *)[heap newBufferWithLength:4096 options:MTLResourceStorageModeShared] : nil;
            if (!heap || (!t && !b)) {
                ++failed;
                continue;
            }
            const MTLPurgeableState was = [heap setPurgeableState:MTLPurgeableStateEmpty];
            const bool purged = gRM->raw->arenaPurged(((RMHeap *)heap)->_arena - 1);
            id tt = t ? [t rmTwin] : @1;
            id bt = b ? [b rmTwin] : @1;
            printf("%s heap: arena %d, purged %d (was %lu): texture twin %s, buffer twin %s\n",
                   mode == MTLStorageModePrivate ? "private" : "shared", ((RMHeap *)heap)->_arena, purged,
                   (unsigned long)was, tt ? "yes" : "NO", bt ? "yes" : "NO");
            failed += !purged || !tt || !bt;
        }
        printf("purgetwin: %d failed\n", failed);
        return failed;
    }
}
