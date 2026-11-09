// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca r16): which textures the facade keeps. 4:2:0 YCbCr textures (YCBCR8_420_2P, 500) over '420v'/'420f'
// surfaces of the texture's size (the pack's conversion states: run with a pack that has them), a padded one and a 10-bit
// 'x420' one it leaves to m2v as m2v reads them; mipmapped textures private and memoryless (chains, when the self-test passed: run with
// RAWCAP_LOG so the mock GPU's 2D engine runs); a 3D texture (m2v's). Each texture's verdict is printed; the device's per-shape
// lines ("raw: texture on m2v: ...") go to the unified log.
// usage: shapecheck <pack> <cache> <nvk dylib>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <initializer_list>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static void missing(const char *, const char *)
{}

static IOSurfaceRef biplanar(OSType fourcc, size_t w, size_t h, size_t bytesPerElement)
{
    const size_t row0 = (w * bytesPerElement + 63) & ~size_t(63),
                 row1 = (((w + 1) / 2) * 2 * bytesPerElement + 63) & ~size_t(63);
    const size_t off1 = (row0 * h + 4095) & ~size_t(4095), bytes = off1 + row1 * ((h + 1) / 2);
    NSDictionary *p0 = @{
        (id)kIOSurfacePlaneWidth : @(w),
        (id)kIOSurfacePlaneHeight : @(h),
        (id)kIOSurfacePlaneBytesPerElement : @(bytesPerElement),
        (id)kIOSurfacePlaneBytesPerRow : @(row0),
        (id)kIOSurfacePlaneOffset : @0,
        (id)kIOSurfacePlaneSize : @(row0 * h)
    };
    NSDictionary *p1 = @{
        (id)kIOSurfacePlaneWidth : @((w + 1) / 2),
        (id)kIOSurfacePlaneHeight : @((h + 1) / 2),
        (id)kIOSurfacePlaneBytesPerElement : @(2 * bytesPerElement),
        (id)kIOSurfacePlaneBytesPerRow : @(row1),
        (id)kIOSurfacePlaneOffset : @(off1),
        (id)kIOSurfacePlaneSize : @(row1 * ((h + 1) / 2))
    };
    NSDictionary *props = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfacePixelFormat : @(fourcc),
        (id)kIOSurfaceAllocSize : @(bytes),
        (id)kIOSurfacePlaneInfo : @[ p0, p1 ]
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4)
            return 2;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (!dev)
            return 1;
        int failed = 0;
        auto verdict = [&](const char *what, id<MTLTexture> t, bool want) {
            const bool raw = t && raw_metal_texture_kept(t);
            printf("%s %-58s %s\n", raw == want ? "OK  " : "FAIL", what, raw ? "raw" : "m2v");
            failed += raw != want;
        };

        struct Case {
            const char *what;
            OSType fourcc;
            size_t sw, sh, bpe, tw, th;
            bool raw;
        } cases[] = {
            {"420v 1280x720, texture the surface's size", '420v', 1280, 720, 1, 1280, 720, true},
            {"420v 1920x1088, texture 1920x1080 (padded: m2v's, as m2v)", '420v', 1920, 1088, 1, 1920, 1080, false},
            {"420f 853x481, texture 853x481 (odd sizes)", '420f', 853, 481, 1, 853, 481, true},
            {"420v 640x360, texture 648x360 (larger than its planes)", '420v', 640, 360, 1, 648, 360, false},
            {"x420 1280x720 (10-bit: m2v's)", 'x420', 1280, 720, 2, 1280, 720, false},
        };

        for (const Case &c : cases) {
            IOSurfaceRef s = biplanar(c.fourcc, c.sw, c.sh, c.bpe);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(500)
                                                                                         width:c.tw
                                                                                        height:c.th
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeShared;
            verdict(c.what, [dev newTextureWithDescriptor:d iosurface:s plane:0], c.raw);
            CFRelease(s);
        }
        for (MTLStorageMode mode : {MTLStorageModePrivate, MTLStorageModeMemoryless}) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:776
                                                                                        height:24
                                                                                     mipmapped:YES];
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            d.storageMode = mode;
            verdict(mode == MTLStorageModePrivate ? "776x24 mipmapped, private (a chain)"
                                                  : "776x24 mipmapped, memoryless (a chain)",
                    [dev newTextureWithDescriptor:d], raw_metal_chains_on());
        }
        for (MTLPixelFormat format : {MTLPixelFormat(43), MTLPixelFormatBGRA8Unorm_sRGB}) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                         width:776
                                                                                        height:24
                                                                                     mipmapped:YES];
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModePrivate;
            verdict(format == MTLPixelFormat(43) ? "776x24 mipmapped BGR5A1Unorm (a chain)"
                                                 : "776x24 mipmapped BGRA8Unorm_sRGB (a chain)",
                    [dev newTextureWithDescriptor:d], true);
        }
        MTLTextureDescriptor *d3 = [MTLTextureDescriptor new];
        d3.textureType = MTLTextureType3D;
        d3.pixelFormat = MTLPixelFormatRGBA16Float;
        d3.width = d3.height = d3.depth = 17;
        d3.usage = MTLTextureUsageShaderRead;
        verdict("17x17x17 RGBA16F 3D (a colour LUT)", [dev newTextureWithDescriptor:d3], true);
        printf("chains %s; shapecheck: %d case(s) failed\n", raw_metal_chains_on() ? "on" : "off", failed);
        return failed ? 1 : 0;
    }
}
