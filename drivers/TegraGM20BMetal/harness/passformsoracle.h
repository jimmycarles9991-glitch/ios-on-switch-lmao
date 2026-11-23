// SPDX-License-Identifier: MIT
// What passformsoracle.mm's colour and zeta cases share: this Mac's device and queue, the cases' library, textures,
// states and readbacks
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace passforms {

inline NSString *const kSource =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct V { float4 p [[position]]; };\n"
    @"vertex V cover(uint v [[vertex_id]], constant float &z [[buffer(0)]]) { V o;\n"
    @"  o.p = float4((v & 1) ? 1.0 : -1.0, (v & 2) ? 1.0 : -1.0, z, 1); return o; }\n"
    @"fragment float4 colour_of(V in [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }\n"
    @"fragment uint4 bits_of(V in [[stage_in]], constant uint4 &c [[buffer(0)]]) { return c; }\n"
    @"fragment float4 sweep(V in [[stage_in]]) { uint2 p = uint2(in.p.xy); float i = float(p.x + 64 * p.y);\n"
    @"  return float4(i / 4095.0, (4095.0 - i) / 4095.0, fract(i * 0.37), 0.5); }\n"
    @"struct D { float depth [[depth(any)]]; };\n"
    @"fragment D depth_of(V in [[stage_in]], constant float &d [[buffer(0)]]) { D o; o.depth = d; return o; }\n"
    @"fragment D depth_by_sample(V in [[stage_in]], uint s [[sample_id]]) { D o; o.depth = 0.2 + 0.1 * float(s);\n"
    @"  return o; }\n"
    @"kernel void read_ms_depth(depth2d_ms<float, access::read> t [[texture(0)]], device float *out [[buffer(0)]],\n"
    @"  uint2 g [[thread_position_in_grid]]) { if (g.x < t.get_width() && g.y < t.get_height())\n"
    @"  for (uint s = 0; s < t.get_num_samples(); s++) out[(g.y * t.get_width() + g.x) * t.get_num_samples() + s] =\n"
    @"  t.read(g, s); }\n";

inline id<MTLDevice> gDevice;
inline id<MTLLibrary> gLibrary;
inline id<MTLCommandQueue> gQueue;

inline id<MTLTexture> make(MTLPixelFormat f, NSUInteger w = 32, NSUInteger h = 32,
                           MTLTextureType type = MTLTextureType2D, NSUInteger samples = 1, NSUInteger layers = 1,
                           NSUInteger levels = 1, MTLStorageMode storage = MTLStorageModePrivate,
                           MTLTextureUsage usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = f;
    d.textureType = type;
    d.width = w;
    d.height = h;
    d.sampleCount = samples;
    d.arrayLength = layers;
    d.mipmapLevelCount = levels;
    d.storageMode = storage;
    d.usage = usage;
    id<MTLTexture> t = [gDevice newTextureWithDescriptor:d];
    if (!t)
        printf("no texture of format %lu type %lu samples %lu\n", (unsigned long)f, (unsigned long)type,
               (unsigned long)samples);
    return t;
}

inline id<MTLRenderPipelineState> state(MTLPixelFormat colour, const char *fragment,
                                        MTLPixelFormat depth = MTLPixelFormatInvalid,
                                        MTLPixelFormat stencil = MTLPixelFormatInvalid, NSUInteger samples = 1,
                                        bool blend = false)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [gLibrary newFunctionWithName:@"cover"];
    d.fragmentFunction = [gLibrary newFunctionWithName:@(fragment)];
    d.colorAttachments[0].pixelFormat = colour;
    if (blend) {
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    d.depthAttachmentPixelFormat = depth;
    d.stencilAttachmentPixelFormat = stencil;
    d.rasterSampleCount = samples;
    NSError *e = nil;
    id<MTLRenderPipelineState> s = [gDevice newRenderPipelineStateWithDescriptor:d error:&e];
    if (!s)
        printf("state %lu/%s: %s\n", (unsigned long)colour, fragment, e.description.UTF8String);
    return s;
}

inline void finish(id<MTLCommandBuffer> cb, const char *what)
{
    [cb commit];
    [cb waitUntilCompleted];
    printf("%s: status %ld error %ld %s\n", what, (long)cb.status, (long)cb.error.code,
           cb.error ? cb.error.localizedDescription.UTF8String : "");
}

inline std::vector<uint8_t> bytes(id<MTLTexture> t, NSUInteger bpp, NSUInteger level = 0, NSUInteger slice = 0,
                                  MTLBlitOption option = MTLBlitOptionNone)
{
    const NSUInteger w = std::max<NSUInteger>(t.width >> level, 1), h = std::max<NSUInteger>(t.height >> level, 1);
    id<MTLBuffer> b = [gDevice newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
    memset(b.contents, 0xee, w * h * bpp);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t
                     sourceSlice:slice
                     sourceLevel:level
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:b
               destinationOffset:0
          destinationBytesPerRow:w * bpp
        destinationBytesPerImage:w * h * bpp
                         options:option];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t *p = static_cast<const uint8_t *>(b.contents);
    return {p, p + w * h * bpp};
}

inline void histogram(const char *what, const std::vector<uint8_t> &b, NSUInteger bpp)
{
    std::map<std::string, int> seen;
    for (size_t i = 0; i + bpp <= b.size(); i += bpp) {
        char k[64] = "";
        for (NSUInteger c = 0; c < bpp; c++)
            snprintf(k + strlen(k), sizeof k - strlen(k), "%02x", b[i + c]);
        seen[k]++;
    }
    printf("  %s:", what);
    int shown = 0;
    for (const auto &[k, n] : seen) {
        if (shown++ == 6) {
            printf(" ... (%zu distinct)", seen.size());
            break;
        }
        printf(" %s x%d", k.c_str(), n);
    }
    printf("\n");
}

void unspecialized();
void unspecializedViews();
void areaStore(NSUInteger side, NSUInteger aw, NSUInteger ah, MTLPixelFormat f = MTLPixelFormatRGBA8Unorm);
void rg8Srgb();
void textureBuffer();

void zetaTwoTextures(bool drawn);
void zetaTwoLevels(bool drawn);
void zetaTwoSlices(bool drawn);
void pairSizes();
void pairSamples();
void mixedPairs();
void stencilView();
void zetaResolves();

}  // namespace passforms
