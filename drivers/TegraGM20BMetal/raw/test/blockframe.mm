// SPDX-License-Identifier: MIT
// raw GM20B path: block-compressed textures (ASTC, BC) through the facade, over the mock GPU (NVDEC_WRITETHROUGH). Private ones
// are raw's, laid out as NVK's optimal image of the format (its m2v twin over the same pages reports the same row stride and
// block height, the block height chosen by the height in texels as NIL chooses it). Metal's copies address texels and move
// whole blocks: a buffer's rows of blocks into the texture, a block-aligned region of it into another, the whole back out,
// each on the raw path and byte for byte what Metal's copy semantics give (HW Test's metal.compressed uploads one ASTC block).
// usage: blockframe <pack> <cache> <nvk dylib>
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

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: blockframe <pack> <cache> <nvk dylib>\n");
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
            unsigned blockW, blockH, blockBytes, width, height;
        };

        const Case cases[] = {{MTLPixelFormatASTC_4x4_LDR, 4, 4, 16, 4, 4},
                              {MTLPixelFormatASTC_4x4_LDR, 4, 4, 16, 84, 84},
                              {MTLPixelFormatASTC_12x12_LDR, 12, 12, 16, 100, 70},
                              {MTLPixelFormatBC1_RGBA, 4, 4, 8, 30, 20},
                              {MTLPixelFormatBC7_RGBAUnorm, 4, 4, 16, 64, 36}};
        for (const Case &c : cases) {
            char what[160];
            snprintf(what, sizeof what, "format %lu %ux%u (blocks %ux%u)", (unsigned long)c.format, c.width, c.height,
                     c.blockW, c.blockH);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.format
                                                                                         width:c.width
                                                                                        height:c.height
                                                                                     mipmapped:NO];
            d.storageMode = MTLStorageModePrivate;
            d.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> a = [dev newTextureWithDescriptor:d], b = [dev newTextureWithDescriptor:d];
            RMTexture *x = (RMTexture *)a;
            struct switch_nvk_image_memory m = {};
            const bool laid = [(id)x isKindOfClass:[RMTexture class]] && x->_rawOk && x->_raw.blockLinear &&
                              x->_raw.tic && native_bridge_metal_texture_memory([x rmTwin], &m) == 0 &&
                              m.row_stride_B == x->_raw.pitch && m.y_log2 == x->_raw.blockHeight &&
                              m.texel_B == c.blockBytes;
            printf("%s %s: raw's, NVK's layout (row stride %u/%u, block height %u/%u)\n", laid ? "OK  " : "FAIL", what,
                   laid ? x->_raw.pitch : 0, m.row_stride_B, laid ? x->_raw.blockHeight : 0, m.y_log2);
            failed += !laid;
            if (!laid)
                continue;

            const unsigned bw = (c.width + c.blockW - 1) / c.blockW, bh = (c.height + c.blockH - 1) / c.blockH,
                           row = bw * c.blockBytes, bytes = row * bh;
            id<MTLBuffer> up = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> down = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            for (unsigned i = 0; i < bytes; i++)
                ((uint8_t *)up.contents)[i] = uint8_t(i * 131u + (i >> 7) * 17u + 5u);
            memset(down.contents, 0xee, bytes);
            // b's blocks: zero, then a's block-aligned region from block (1, 1) of up to 2 x 2 blocks, at block (0, 0)
            std::vector<uint8_t> want(bytes, 0);
            const unsigned rw = std::min(2u, bw - std::min(1u, bw - 1)), rh = std::min(2u, bh - std::min(1u, bh - 1)),
                           ox = std::min(1u, bw - 1), oy = std::min(1u, bh - 1);
            for (unsigned y = 0; y < rh; y++)
                memcpy(&want[y * row], (uint8_t *)up.contents + (oy + y) * row + ox * c.blockBytes, rw * c.blockBytes);
            id<MTLBuffer> zero = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            memset(zero.contents, 0, bytes);
            const MTLSize whole = MTLSizeMake(c.width, c.height, 1);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            for (auto [from, to] : {std::pair{up, a}, std::pair{zero, b}})
                [e copyFromBuffer:from
                           sourceOffset:0
                      sourceBytesPerRow:row
                    sourceBytesPerImage:bytes
                             sourceSize:whole
                              toTexture:to
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
            [e copyFromTexture:a
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(ox * c.blockW, oy * c.blockH, 0)
                       sourceSize:MTLSizeMake(std::min(rw * c.blockW, c.width - ox * c.blockW),
                                              std::min(rh * c.blockH, c.height - oy * c.blockH), 1)
                        toTexture:b
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [e endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            id<MTLBuffer> region = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            memset(region.contents, 0xee, bytes);
            id<MTLCommandBuffer> rb = [q commandBuffer];
            id<MTLBlitCommandEncoder> r = [rb blitCommandEncoder];
            for (auto [from, to] : {std::pair{a, down}, std::pair{b, region}})
                [r copyFromTexture:from
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:whole
                                    toBuffer:to
                           destinationOffset:0
                      destinationBytesPerRow:row
                    destinationBytesPerImage:bytes];
            [r endEncoding];
            [rb commit];
            [rb waitUntilCompleted];
            const bool raw = [path(cb) isEqualToString:@"raw"] && [path(rb) isEqualToString:@"raw"];
            const bool same = !memcmp(up.contents, down.contents, bytes),
                       part = !memcmp(region.contents, want.data(), bytes);
            printf("%s %s: its blocks in and out, and a region into another, on raw (%s / %s)%s%s\n",
                   raw && same && part ? "OK  " : "FAIL", what, path(cb).UTF8String, path(rb).UTF8String,
                   same ? "" : " [whole differs]", part ? "" : " [region differs]");
            failed += !(raw && same && part);
        }
        printf("raw facade: %d case(s) failed\n", failed);
        return failed ? 1 : 0;
    }
}
