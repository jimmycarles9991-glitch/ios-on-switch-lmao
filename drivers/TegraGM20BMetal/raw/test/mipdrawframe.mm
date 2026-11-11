// SPDX-License-Identifier: MIT
// raw GM20B path: generateMipmapsForTexture: of the formats the 2D engine does not filter as Metal does (Core Animation's
// BGR5A1Unorm surfaces, sRGB, packed, 16- and 32-bit channels) on the 3D engine, over the mock GPU (NVDEC_PROBE prints each
// draw): every command buffer stays raw, and the chain gets one draw of the pack's mip state a level, into level l + 1 at its
// size. The pixels are the device's to check (HW Test gpu.mipmaps). A BGRA8Unorm chain stays on the 2D engine (no draw).
// usage: mipdrawframe <pack> <cache> <nvk dylib>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <string>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static NSString *path(id<MTLCommandBuffer> cb)
{
    const SEL sel = NSSelectorFromString(@"switchRawPath");
    return [(id)cb respondsToSelector:sel] ? ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel) : @"device";
}

// The draws the raw path made in the one command buffer since the last call (the rates line's d=, draws a command buffer)
static unsigned drawsSince()
{
    char line[4096];
    if (!raw_metal_rates(line, sizeof line))
        return 0;
    const char *at = strstr(line, " d=");
    return at ? unsigned(lround(atof(at + 3))) : 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: mipdrawframe <pack> <cache> <nvk dylib>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVDEC_WRITETHROUGH", "1", 0);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s %s\n", nmb_last_error(), why.UTF8String);
            return 1;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int failed = 0;

        struct Case {
            MTLPixelFormat format;
            unsigned width, height, levels;
            bool drawn;
        };

        const Case cases[] = {
            {MTLPixelFormatBGR5A1Unorm, 776, 24, 10, true},    {MTLPixelFormatBGRA8Unorm_sRGB, 64, 64, 7, true},
            {MTLPixelFormatRGBA8Unorm_sRGB, 100, 60, 7, true}, {MTLPixelFormatB5G6R5Unorm, 32, 32, 6, true},
            {MTLPixelFormatRG11B10Float, 40, 40, 6, true},     {MTLPixelFormatRGBA32Float, 16, 16, 5, true},
            {MTLPixelFormatBGRA8Unorm, 64, 64, 7, false}};
        drawsSince();
        for (const Case &c : cases) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.format
                                                                                         width:c.width
                                                                                        height:c.height
                                                                                     mipmapped:YES];
            d.mipmapLevelCount = c.levels;
            d.storageMode = MTLStorageModePrivate;
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            id<MTLTexture> t = [dev newTextureWithDescriptor:d];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b generateMipmapsForTexture:t];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            usleep(10000);
            const unsigned draws = drawsSince();
            const bool raw = [path(cb) isEqualToString:@"raw"] && cb.status == MTLCommandBufferStatusCompleted;
            const bool ok = raw && draws == (c.drawn ? c.levels - 1 : 0);
            printf("%s format %lu %ux%u, %u levels: %s, %u draws (want %u)\n", ok ? "ok  " : "FAIL",
                   (unsigned long)c.format, c.width, c.height, c.levels, path(cb).UTF8String, draws,
                   c.drawn ? c.levels - 1 : 0);
            failed += !ok;
        }
        printf("%s: mipdrawframe\n", failed ? "FAIL" : "PASS");
        return failed ? 1 : 0;
    }
}
