// SPDX-License-Identifier: MIT
// passformsoracle.mm's colour cases: format 263 and RG8Unorm_sRGB targets, texture buffer targets, and how far a
// Clear under a render area smaller than its target reaches
#include "passformsoracle.h"

namespace passforms {

// MTLPixelFormatUnspecialized (263) as a texture and a colour target
void unspecialized()
{
    for (MTLStorageMode storage : {MTLStorageModePrivate, MTLStorageModeShared}) {
        id<MTLTexture> t = make(MTLPixelFormat(263), 16, 16, MTLTextureType2D, 1, 1, 1, storage);
        printf("263 texture (storage %lu): %s\n", (unsigned long)storage, t ? "made" : "nil");
        if (!t)
            continue;
        printf("  pixelFormat %lu, allocated %lu\n", (unsigned long)t.pixelFormat, (unsigned long)t.allocatedSize);
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t;
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
        finish(cb, "263 cleared (.25, .5, .75, 1)");
        histogram("bytes", bytes(t, 4), 4);
        for (MTLPixelFormat sf :
             {MTLPixelFormatRGBA8Unorm, MTLPixelFormatR32Uint, MTLPixelFormatR32Float, MTLPixelFormat(263)}) {
            id<MTLRenderPipelineState> st = state(sf, sf == MTLPixelFormatR32Uint ? "bits_of" : "colour_of");
            if (!st)
                continue;
            id<MTLCommandBuffer> cb2 = [gQueue commandBuffer];
            p.colorAttachments[0].loadAction = MTLLoadActionLoad;
            id<MTLRenderCommandEncoder> e = [cb2 renderCommandEncoderWithDescriptor:p];
            [e setRenderPipelineState:st];
            const float z = 0.5f;
            const float c[4] = {0.125f, 0.25f, 0.375f, 0.5f};
            const uint32_t u[4] = {0x12345678u, 2, 3, 4};
            [e setVertexBytes:&z length:4 atIndex:0];
            [e setFragmentBytes:sf == MTLPixelFormatR32Uint ? (const void *)u : (const void *)c length:16 atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [e endEncoding];
            char what[64];
            snprintf(what, sizeof what, "263 drawn by a state of format %lu", (unsigned long)sf);
            finish(cb2, what);
            histogram("bytes", bytes(t, 4), 4);
        }
    }
}

// Format 263 by a view and over a buffer; a texture buffer wider than a render target's 16384
void unspecializedViews()
{
    id<MTLTexture> t = make(MTLPixelFormatR32Uint, 16, 16, MTLTextureType2D, 1, 1, 1, MTLStorageModeShared,
                            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView);
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:MTLPixelFormat(263)];
    printf("263 view of R32Uint: %s\n", v ? "made" : "nil");
    id<MTLBuffer> b = [gDevice newBufferWithLength:16 * 64 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(263)
                                                                                 width:16
                                                                                height:16
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> bt = [b newTextureWithDescriptor:d offset:0 bytesPerRow:64];
    printf("263 over a buffer: %s\n", bt ? "made" : "nil");
    const NSUInteger n = 20000;
    id<MTLBuffer> wide = [gDevice newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
    memset(wide.contents, 0x11, n * 4);
    MTLTextureDescriptor *w = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:n
                                                                           resourceOptions:MTLResourceStorageModeShared
                                                                                     usage:MTLTextureUsageRenderTarget];
    id<MTLTexture> wt = [wide newTextureWithDescriptor:w offset:0 bytesPerRow:n * 4];
    printf("texture buffer of %lu: %s\n", (unsigned long)n, wt ? "made" : "nil");
    if (!wt)
        return;
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = wt;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, "wide texture buffer cleared red");
    const uint32_t *c = static_cast<const uint32_t *>(wide.contents);
    NSUInteger red = 0, last = 0;
    for (NSUInteger i = 0; i < n; i++)
        if (c[i] == 0xff0000ffu) {
            red++;
            last = i;
        }
    printf("  %lu red, last at %lu\n", (unsigned long)red, (unsigned long)last);
}

// A one-sample Clear + Store under renderTargetWidth 40, renderTargetHeight 24 of a 64x64 texture held 0x11: which texels
// the clear reaches (Apple's tiles, or the area alone)
void areaStore(NSUInteger side, NSUInteger aw, NSUInteger ah, MTLPixelFormat f)
{
    id<MTLTexture> t = make(f, side, side);
    for (int pass = 0; pass < 2; pass++) {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t;
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[0].clearColor =
            pass ? MTLClearColorMake(1, 0, 0, 1)
                 : MTLClearColorMake(0x11 / 255.0, 0x11 / 255.0, 0x11 / 255.0, 0x11 / 255.0);
        if (pass) {
            p.renderTargetWidth = aw;
            p.renderTargetHeight = ah;
        }
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
        finish(cb, pass ? "clear red under 40x24" : "fill 0x11");
    }
    const NSUInteger bpp = f == MTLPixelFormatRGBA16Float ? 8 : 4;
    const std::vector<uint8_t> b = bytes(t, bpp);
    int red = 0, x1 = 0, y1 = 0;
    for (int y = 0; y < int(side); y++)
        for (int x = 0; x < int(side); x++)
            if (b[bpp * (y * side + x)] != 0x11 && b[bpp * (y * side + x)] != 0) {
                red++;
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
    printf("  %lux%lu format %lu under %lux%lu: cleared %d texels, bounding [0, %d) x [0, %d)\n", (unsigned long)side,
           (unsigned long)side, (unsigned long)f, (unsigned long)aw, (unsigned long)ah, red, x1, y1);
}

// RG8Unorm_sRGB against RGBA8Unorm_sRGB's first two bytes: a sweep of drawn values, clears, and blends over loaded bytes
void rg8Srgb()
{
    id<MTLTexture> rg = make(MTLPixelFormatRG8Unorm_sRGB, 64, 64), rgba = make(MTLPixelFormatRGBA8Unorm_sRGB, 64, 64);
    if (!rg || !rgba)
        return;
    const auto compare = [&](const char *what) {
        const std::vector<uint8_t> a = bytes(rg, 2), b = bytes(rgba, 4);
        int off = 0, maxOff = 0;
        for (size_t i = 0; i < 64 * 64; i++)
            for (int c = 0; c < 2; c++) {
                const int d = std::abs(int(a[2 * i + c]) - int(b[4 * i + c]));
                off += d != 0;
                maxOff = std::max(maxOff, d);
            }
        printf("  %s: %d of 8192 channels differ, max %d; texel 0 rg %02x%02x rgba %02x%02x%02x%02x\n", what, off,
               maxOff, a[0], a[1], b[0], b[1], b[2], b[3]);
    };
    const auto pass = [&](id<MTLTexture> t, MTLLoadAction load, MTLClearColor k, const char *fragment, bool blend,
                          const float *colour) {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t;
        p.colorAttachments[0].loadAction = load;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[0].clearColor = k;
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        if (fragment) {
            [e setRenderPipelineState:state(t.pixelFormat, fragment, MTLPixelFormatInvalid, MTLPixelFormatInvalid, 1,
                                            blend)];
            const float z = 0.5f;
            [e setVertexBytes:&z length:4 atIndex:0];
            if (colour)
                [e setFragmentBytes:colour length:16 atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    };
    for (id<MTLTexture> t : {rg, rgba})
        pass(t, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 1), "sweep", false, nullptr);
    compare("sweep drawn");
    const double clears[][2] = {{0.5, 0.25}, {0.0031, 0.04045}, {0.2140, 0.7354}, {1.0 / 3, 2.0 / 3}, {0.99, 0.001}};
    int clearsOff = 0;
    for (const auto &k : clears) {
        for (id<MTLTexture> t : {rg, rgba})
            pass(t, MTLLoadActionClear, MTLClearColorMake(k[0], k[1], 0.5, 1), nullptr, false, nullptr);
        const std::vector<uint8_t> a = bytes(rg, 2), b = bytes(rgba, 4);
        printf("  clear (%g, %g): rg %02x%02x rgba %02x%02x\n", k[0], k[1], a[0], a[1], b[0], b[1]);
        clearsOff += a[0] != b[0] || a[1] != b[1];
    }
    printf("  clears differing: %d of %zu\n", clearsOff, sizeof clears / sizeof clears[0]);
    for (id<MTLTexture> t : {rg, rgba})
        pass(t, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 1), "sweep", false, nullptr);
    const float over[4] = {0.8f, 0.1f, 0.3f, 0.37f};
    for (id<MTLTexture> t : {rg, rgba})
        pass(t, MTLLoadActionLoad, MTLClearColorMake(0, 0, 0, 1), "colour_of", true, over);
    compare("sweep, loaded, then blended (src alpha .37)");
}

// A texture buffer (MTLTextureTypeTextureBuffer) as a colour target
void textureBuffer()
{
    id<MTLBuffer> b = [gDevice newBufferWithLength:256 * 4 options:MTLResourceStorageModeShared];
    memset(b.contents, 0x11, 256 * 4);
    MTLTextureDescriptor *d = [MTLTextureDescriptor
        textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:256
                               resourceOptions:MTLResourceStorageModeShared
                                         usage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
    id<MTLTexture> t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:256 * 4];
    printf("texture buffer with render target usage: %s\n", t ? "made" : "nil");
    if (!t) {
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:256 * 4];
        printf("texture buffer read/write: %s\n", t ? "made" : "nil");
    }
    if (!t)
        return;
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, "texture buffer cleared red");
    const uint8_t *c = static_cast<const uint8_t *>(b.contents);
    histogram("buffer", std::vector<uint8_t>(c, c + 256 * 4), 4);
}

}  // namespace passforms
