// SPDX-License-Identifier: MIT
// raw GM20B path: 1D array textures and swizzled views through the facade, over the mock GPU (NVDEC_WRITETHROUGH). WeatherPoster's
// VFX makes a private RGBA16Float 1D array of one slice, uploads it with a buffer copy and samples it through
// newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:, which m2v's texture answers nil to (a null dereference in
// VFX). Here the array is raw's, its upload and readback take the raw path, the swizzled views (1D and 1D array, of every
// MTLTextureSwizzle) are raw's with the swizzle they were made with, and texture header word 0 takes each swizzle as NVK's
// view does (nil_view swizzle over the format's sources: Zero, One of the format's kind, a source component).
// usage: swizzleframe <pack> <cache> <nvk dylib>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vector>
#include "../runtime/RawMetalInternal.h"
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

// X..W_SOURCE of header word 0
static uint32_t sources(uint32_t word0)
{
    return word0 >> 19 & 0xfff;
}

static uint32_t of(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    return x | y << 3 | z << 6 | w << 9;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: swizzleframe <pack> <cache> <nvk dylib>\n");
            return 2;
        }
        int failed = 0;
        auto expect = [&](bool ok, const char *what) {
            printf("%s %s\n", ok ? "OK  " : "FAIL", what);
            failed += !ok;
        };
        // RGBA16Float (R G B A sources, FLOAT), R32Float (R 0 0 ONE_FLOAT), R16Uint (R 0 0 ONE_INT)
        const uint32_t rgba16f = 0x58d7ff83, r32f = 0x7017ff8f, r16u = 0x6014921b;
        const uint8_t bgra[4] = {4, 3, 2, 5}, rrr1[4] = {2, 2, 2, 1}, a0g0[4] = {5, 0, 3, 0};
        expect(
            sources(raw::swizzledWord0(rgba16f, bgra)) == of(4, 3, 2, 5) &&
                sources(raw::swizzledWord0(rgba16f, rrr1)) == of(2, 2, 2, 7) &&
                sources(raw::swizzledWord0(r32f, a0g0)) == of(7, 0, 0, 0) &&
                sources(raw::swizzledWord0(r16u, rrr1)) == of(2, 2, 2, 6) &&
                raw::swizzledWord0(rgba16f, raw::kIdentitySwizzle) == rgba16f &&
                (raw::swizzledWord0(r32f, bgra) & ~(0xfffu << 19)) == (r32f & ~(0xfffu << 19)),
            "header word 0: each channel from the format's own source, its kind of one, or zero; nothing else moved");

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
        const NSUInteger width = 256, row = width * 8;
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = MTLTextureType1DArray;
        d.pixelFormat = MTLPixelFormatRGBA16Float;
        d.width = width;
        d.arrayLength = 1;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> array = [dev newTextureWithDescriptor:d];
        RMTexture *t = (RMTexture *)array;
        expect([(id)t isKindOfClass:[RMTexture class]] && t->_rawOk && t->_raw.type == raw::fx::kTexOneDArray &&
                   t->_raw.tic,
               "a private RGBA16Float 1D array of one slice is raw's, under a ONE_D_ARRAY header");
        struct switch_nvk_image_memory m = {};
        expect(native_bridge_metal_texture_memory([t rmTwin], &m) == 0 && m.tiled && m.row_stride_B == t->_raw.pitch &&
                   m.y_log2 == t->_raw.blockHeight && m.z_log2 == 0 && m.array_stride_B == t->_raw.layerBytes,
               "m2v's twin over the same pages is NVK's 1D array image of the same layout");

        id<MTLBuffer> up = [dev newBufferWithLength:row options:MTLResourceStorageModeShared];
        id<MTLBuffer> down = [dev newBufferWithLength:row options:MTLResourceStorageModeShared];
        for (NSUInteger i = 0; i < row; i++)
            ((uint8_t *)up.contents)[i] = uint8_t(i * 131u + 7u);
        memset(down.contents, 0xee, row);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromBuffer:up
                   sourceOffset:0
              sourceBytesPerRow:row
            sourceBytesPerImage:row
                     sourceSize:MTLSizeMake(width, 1, 1)
                      toTexture:array
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b copyFromTexture:array
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, 1, 1)
                            toBuffer:down
                   destinationOffset:0
              destinationBytesPerRow:row
            destinationBytesPerImage:row];
        [b endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        expect([path(cb) isEqualToString:@"raw"] && !memcmp(up.contents, down.contents, row),
               "its upload (rows of 2048 bytes) and readback take the raw path and keep its texels");

        const MTLTextureSwizzleChannels swizzles[] = {
            MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed,
                                          MTLTextureSwizzleAlpha),
            MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                          MTLTextureSwizzleOne),
            MTLTextureSwizzleChannelsMake(MTLTextureSwizzleZero, MTLTextureSwizzleOne, MTLTextureSwizzleAlpha,
                                          MTLTextureSwizzleGreen)};
        for (MTLTextureType type : {MTLTextureType1D, MTLTextureType1DArray})
            for (const MTLTextureSwizzleChannels &s : swizzles) {
                RMTexture *v = (RMTexture *)[array newTextureViewWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                     textureType:type
                                                                          levels:NSMakeRange(0, 1)
                                                                          slices:NSMakeRange(0, 1)
                                                                         swizzle:s];
                const MTLTextureSwizzleChannels got = v ? [(id<MTLTexture>)v swizzle] : MTLTextureSwizzleChannels{};
                const uint8_t want[4] = {uint8_t(s.red), uint8_t(s.green), uint8_t(s.blue), uint8_t(s.alpha)};
                char what[160];
                snprintf(what, sizeof what,
                         "a %s view swizzled %u %u %u %u: raw's, its own header, the swizzle it was made with",
                         type == MTLTextureType1D ? "1D" : "1D array", want[0], want[1], want[2], want[3]);
                expect(v && v->_rawOk && v->_raw.tic && v->_raw.tic != t->_raw.tic &&
                           !memcmp(v->_raw.swizzle, want, 4) && got.red == s.red && got.green == s.green &&
                           got.blue == s.blue && got.alpha == s.alpha,
                       what);
            }
        expect(!memcmp(t->_raw.swizzle, raw::kIdentitySwizzle, 4), "the viewed texture keeps its own identity swizzle");
        printf("raw facade: %d case(s) failed\n", failed);
        return failed ? 1 : 0;
    }
}
