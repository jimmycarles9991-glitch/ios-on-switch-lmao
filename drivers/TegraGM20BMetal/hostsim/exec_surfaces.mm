// SPDX-License-Identifier: MIT
// Each of m2v's submissions names the host imports its encoders use to the kernel (ExecSurfaces, method 17), through the
// bridge's hook into NVK (switch_nvk_submit_surfaces), over mock_iokit.c, which keeps the last record:
//  - a pass that renders into an IOSurface lists the surface's import as written;
//  - a copy out of it lists it as read alone;
//  - a command buffer of the device's own memory alone lists nothing, and is no blanket submission.
// usage: exec_surfaces <host libvulkan_nouveau.dylib> <cache dir>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"
#include "TegraGM20BABI.h"
#include "bridge/NativeMetalObjects.h"
#include "mock_iokit.h"

namespace {
int failures;

void missing(const char *, const char *)
{}

void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok)
        failures++;
}

IOSurfaceRef surface(unsigned w, unsigned h)
{
    NSDictionary *p = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(w * 4),
        (id)kIOSurfacePixelFormat : @(0x42475241u),  // 'BGRA'
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

// The last record after `cb` ran, and whether one came for it
bool recordOf(id<MTLCommandBuffer> cb, mock_exec_surfaces &r)
{
    mock_exec_surfaces before;
    mock_iokit_exec_surfaces(&before);
    [cb commit];
    [cb waitUntilCompleted];
    mock_iokit_exec_surfaces(&r);
    return r.lists > before.lists;
}

uint32_t accessOf(const mock_exec_surfaces &r, uint64_t handle)
{
    for (uint32_t i = 0; i < r.count; i++)
        if (r.uses[i].handle == handle)
            return r.uses[i].access;
    return 0;
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: exec_surfaces <host libvulkan_nouveau.dylib> <cache dir>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        const unsigned w = 64, h = 32;
        IOSurfaceRef s = surface(w, h);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
        const uintptr_t page = uintptr_t(getpagesize());
        const uint64_t handle = mock_iokit_import_handle((void *)(uintptr_t(IOSurfaceGetBaseAddress(s)) & ~(page - 1)));
        expect(t && handle, "the surface's texture is over a BoImport");

        mock_exec_surfaces r;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = t;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
        const bool rendered = recordOf(cb, r);
        expect(rendered && !r.flags && (accessOf(r, handle) & TegraGM20BSurfaceWrite),
               "a pass into the surface lists its import as written");

        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> p = [dev newTextureWithDescriptor:d];
        cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t toTexture:p];
        [b endEncoding];
        const bool copied = recordOf(cb, r);
        expect(copied && !r.flags && accessOf(r, handle) == TegraGM20BSurfaceRead,
               "a copy out of the surface lists its import as read alone");

        id<MTLBuffer> own = [dev newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
        cb = [queue commandBuffer];
        b = [cb blitCommandEncoder];
        [b fillBuffer:own range:NSMakeRange(0, own.length) value:7];
        [b endEncoding];
        const bool filled = recordOf(cb, r);
        expect(filled && !r.count && !r.flags, "a command buffer of the device's own memory lists no import");
        CFRelease(s);
        printf("%s\n", failures ? "FAILED" : "ALL OK");
        return failures ? 1 : 0;
    }
}
