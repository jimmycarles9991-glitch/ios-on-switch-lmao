// SPDX-License-Identifier: MIT
// xroracle.mm's Core Animation passes: each of QuartzCore's extended-range passes that samples its source (the blur chain's
// downsamples and narrow blurs, the APL dimming), drawn on this Mac's GPU into a BGRA10_XR target twice: sampling the source of
// each extended-range form natively, and sampling its stand-in (pack/xr.metal's RGBA16Float image of the same codes) as the raw
// path does. Inputs come from the functions' reflection: identity matrices, the vertices of a full-target quad with texture
// coordinates over the source, and deterministic values elsewhere. Prints how many stored codes differ and by how much.
#pragma once
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <cstdlib>
#include <random>
#include <vector>

namespace xroracle {

struct CaPass {
    const char *vertex, *fragment;
};

inline const CaPass kCaPasses[] = {
    {"downsample_blur_vert_lph", "downsample_4_frag_lph"},   {"downsample_blur_vert_lph", "downsample_8_frag_lph"},
    {"downsample_blur_vert_lph", "narrow_blur_11_frag_lph"}, {"downsample_blur_vert_lph", "narrow_blur_15_frag_lph"},
    {"downsample_blur_vert_lph", "narrow_blur_19_frag_lph"}, {"downsample_blur_vert_lph", "narrow_blur_23_frag_lph"},
    {"downsample_blur_vert_lph", "narrow_blur_27_frag_lph"}, {"std_vert1_lph", "apply_apl_dimming_lph"},
};

constexpr NSUInteger kSrcW = 61, kSrcH = 47, kDstW = 48, kDstH = 36;

inline void finish(id<MTLCommandBuffer> cb)
{
    [cb commit];
    [cb waitUntilCompleted];
}

inline uint16_t halfBits(float v)
{
    const _Float16 h = _Float16(v);
    uint16_t b;
    memcpy(&b, &h, 2);
    return b;
}

// A buffer for binding `b`: a matrix's identity, else deterministic values in [0, 1) of the binding's scalar type, offsets
// (a member named "offset") a few texels apart
inline id<MTLBuffer> argument(id<MTLDevice> dev, id<MTLBufferBinding> b)
{
    std::vector<uint8_t> bytes(4096, 0);
    NSString *name = b.name;
    if ([name containsString:@"matrix"] || [name hasPrefix:@"texmat"]) {
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(bytes.data(), m, sizeof m);
        return [dev newBufferWithBytes:bytes.data() length:bytes.size() options:MTLResourceStorageModeShared];
    }
    auto fill = [&](size_t at, size_t end, MTLDataType type, bool offsets) {
        for (size_t i = 0; at < end && at < bytes.size(); i++) {
            const float v = offsets ? (i & 1 ? 1.0f : -1.0f) * (0.6f + 0.9f * float(i % 5)) / float(kSrcW)
                                    : 0.05f + 0.9f * float((i * 37) % 97) / 97.0f;
            if (type == MTLDataTypeHalf || type == MTLDataTypeHalf2 || type == MTLDataTypeHalf3 ||
                type == MTLDataTypeHalf4) {
                const uint16_t h = halfBits(v);
                memcpy(&bytes[at], &h, 2);
                at += 2;
            } else {
                memcpy(&bytes[at], &v, 4);
                at += 4;
            }
        }
    };
    if (MTLStructType *s = b.bufferStructType) {
        NSArray<MTLStructMember *> *members = s.members;
        for (NSUInteger i = 0; i < members.count; i++) {
            MTLStructMember *m = members[i];
            const size_t end = i + 1 < members.count ? members[i + 1].offset : m.offset + 256;
            MTLDataType type = m.dataType == MTLDataTypeArray ? m.arrayType.elementType : m.dataType;
            fill(m.offset, end, type, [m.name isEqualToString:@"offset"]);
        }
    } else {
        fill(0, 256, b.bufferDataType, false);
    }
    return [dev newBufferWithBytes:bytes.data() length:bytes.size() options:MTLResourceStorageModeShared];
}

// The source's codes: random over every code, BGRA10_XR's alpha over [0, 1]'s codes, the two-plane alpha byte over its range
struct Source {
    id<MTLTexture> xr, codes;
    const char *convert, *name;
    CVPixelBufferRef surface = nullptr;
};

inline Source source(id<MTLDevice> dev, int which, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint32_t> code(0, 1023), alpha(384, 894), byte(0, 255);
    const NSUInteger n = kSrcW * kSrcH;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.width = kSrcW;
    d.height = kSrcH;
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    Source s;
    if (which == 0) {
        std::vector<uint32_t> w(n);
        for (uint32_t &x : w)
            x = 0xC0000000u | code(rng) << 20 | code(rng) << 10 | code(rng);
        d.pixelFormat = MTLPixelFormatBGR10_XR;
        s.xr = [dev newTextureWithDescriptor:d];
        d.pixelFormat = MTLPixelFormatR32Uint;
        s.codes = [dev newTextureWithDescriptor:d];
        for (id<MTLTexture> t : @[ s.xr, s.codes ])
            [t replaceRegion:MTLRegionMake2D(0, 0, kSrcW, kSrcH)
                 mipmapLevel:0
                   withBytes:w.data()
                 bytesPerRow:kSrcW * 4];
        s.convert = "xr_bgr10_frag";
        s.name = "BGR10_XR";
    } else if (which == 1) {
        std::vector<uint16_t> f(n * 4);
        for (size_t i = 0; i < n; i++)
            for (int k = 0; k < 4; k++)
                f[i * 4 + k] = uint16_t((k == 3 ? alpha(rng) : code(rng)) << 6);
        d.pixelFormat = MTLPixelFormatBGRA10_XR;
        s.xr = [dev newTextureWithDescriptor:d];
        d.pixelFormat = MTLPixelFormatRGBA16Uint;
        s.codes = [dev newTextureWithDescriptor:d];
        for (id<MTLTexture> t : @[ s.xr, s.codes ])
            [t replaceRegion:MTLRegionMake2D(0, 0, kSrcW, kSrcH)
                 mipmapLevel:0
                   withBytes:f.data()
                 bytesPerRow:kSrcW * 8];
        s.convert = "xr_bgra10_frag";
        s.name = "BGRA10_XR";
    } else {
        NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
        if (CVPixelBufferCreate(nullptr, kSrcW, kSrcH, 'b3a8', (__bridge CFDictionaryRef)attributes, &s.surface))
            return s;
        CVPixelBufferLockBaseAddress(s.surface, 0);
        auto *words = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(s.surface, 0);
        auto *bytes = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(s.surface, 1);
        const size_t wr = CVPixelBufferGetBytesPerRowOfPlane(s.surface, 0),
                     br = CVPixelBufferGetBytesPerRowOfPlane(s.surface, 1);
        std::vector<uint32_t> pairs(n * 2);
        for (NSUInteger y = 0; y < kSrcH; y++)
            for (NSUInteger x = 0; x < kSrcW; x++) {
                const uint32_t w = code(rng) << 20 | code(rng) << 10 | code(rng), a = byte(rng);
                memcpy(words + y * wr + x * 4, &w, 4);
                bytes[y * br + x] = uint8_t(a);
                pairs[(y * kSrcW + x) * 2] = w;
                pairs[(y * kSrcW + x) * 2 + 1] = a;
            }
        CVPixelBufferUnlockBaseAddress(s.surface, 0);
        d.pixelFormat = MTLPixelFormat(550);
        s.xr = [dev newTextureWithDescriptor:d iosurface:CVPixelBufferGetIOSurface(s.surface) plane:0];
        d.pixelFormat = MTLPixelFormatRG32Uint;
        s.codes = [dev newTextureWithDescriptor:d];
        [s.codes replaceRegion:MTLRegionMake2D(0, 0, kSrcW, kSrcH)
                   mipmapLevel:0
                     withBytes:pairs.data()
                   bytesPerRow:kSrcW * 8];
        s.convert = "xr_planes_frag";
        s.name = "RGB10A8_2P_XR10";
    }
    return s;
}

inline id<MTLTexture> convert(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> xr, const Source &s)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [xr newFunctionWithName:@"xr_vert"];
    d.fragmentFunction = [xr newFunctionWithName:@(s.convert)];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:nil];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                  width:kSrcW
                                                                                 height:kSrcH
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = [dev newTextureWithDescriptor:td];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = out;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:pass];
    [r setRenderPipelineState:p];
    [r setFragmentTexture:s.codes atIndex:0];
    [r drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [r endEncoding];
    finish(cb);
    return out;
}

// The pass drawn with `src` as its texture0 into a BGRA10_XR target: the target's 16-bit fields
inline std::vector<uint16_t> draw(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> p,
                                  MTLRenderPipelineReflection *refl, id<MTLTexture> src, id<MTLTexture> other)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA10_XR
                                                                                  width:kDstW
                                                                                 height:kDstH
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> out = [dev newBufferWithLength:kDstW * kDstH * 8 options:MTLResourceStorageModeShared];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> linear = [dev newSamplerStateWithDescriptor:sd];
    // a0 position, a1 and a2 texture coordinates, a3 a half4 color (Core Animation's vertex layout, stride 48)
    float v[4][12] = {};
    for (int i = 0; i < 4; i++) {
        v[i][0] = i & 1 ? 1.0f : -1.0f;
        v[i][1] = i & 2 ? -1.0f : 1.0f;
        v[i][3] = 1.0f;
        v[i][4] = v[i][6] = i & 1 ? 1.0f : 0.0f;
        v[i][5] = v[i][7] = i & 2 ? 1.0f : 0.0f;
        const uint16_t one = halfBits(1.0f);
        memcpy(&v[i][8], &one, 2);
        memcpy(reinterpret_cast<uint8_t *>(&v[i][8]) + 2, &one, 2);
        memcpy(&v[i][9], &one, 2);
        memcpy(reinterpret_cast<uint8_t *>(&v[i][9]) + 2, &one, 2);
    }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:pass];
    [r setRenderPipelineState:p];
    [r setVertexBytes:v length:sizeof v atIndex:1];
    for (id<MTLBinding> b in refl.vertexBindings)
        if (b.type == MTLBindingTypeBuffer && b.index != 1)
            [r setVertexBuffer:argument(dev, (id<MTLBufferBinding>)b) offset:0 atIndex:b.index];
    for (id<MTLBinding> b in refl.fragmentBindings) {
        if (b.type == MTLBindingTypeBuffer)
            [r setFragmentBuffer:argument(dev, (id<MTLBufferBinding>)b) offset:0 atIndex:b.index];
        else if (b.type == MTLBindingTypeTexture)
            [r setFragmentTexture:[b.name isEqualToString:@"texture0"] ? src : other atIndex:b.index];
        else if (b.type == MTLBindingTypeSampler)
            [r setFragmentSamplerState:linear atIndex:b.index];
    }
    [r drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [r endEncoding];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(kDstW, kDstH, 1)
                        toBuffer:out
               destinationOffset:0
          destinationBytesPerRow:kDstW * 8
        destinationBytesPerImage:kDstW * kDstH * 8];
    [bl endEncoding];
    finish(cb);
    const uint16_t *f = (const uint16_t *)out.contents;
    return std::vector<uint16_t>(f, f + kDstW * kDstH * 4);
}

// Every pass over each form's source: the codes stored with the stand-in sampled against those with the source sampled natively
inline int passes(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> qc, id<MTLLibrary> xr)
{
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    id<MTLTexture> other = [dev newTextureWithDescriptor:od];
    std::vector<uint32_t> noise(64 * 64);
    for (size_t i = 0; i < noise.size(); i++)
        noise[i] = uint32_t(i * 2654435761u);
    [other replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:noise.data() bytesPerRow:64 * 4];
    int bad = 0;
    for (const CaPass &c : kCaPasses) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [qc newFunctionWithName:@(c.vertex)];
        d.fragmentFunction = [qc newFunctionWithName:@(c.fragment)];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA10_XR;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        const MTLVertexFormat formats[4] = {MTLVertexFormatFloat4, MTLVertexFormatFloat2, MTLVertexFormatFloat2,
                                            MTLVertexFormatHalf4};
        const NSUInteger offsets[4] = {0, 16, 24, 32};
        for (int a = 0; a < 4; a++) {
            vd.attributes[a].format = formats[a];
            vd.attributes[a].offset = offsets[a];
            vd.attributes[a].bufferIndex = 1;
        }
        vd.layouts[1].stride = 48;
        d.vertexDescriptor = vd;
        MTLAutoreleasedRenderPipelineReflection refl = nil;
        NSError *e = nil;
        id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d
                                                                         options:MTLPipelineOptionBindingInfo
                                                                      reflection:&refl
                                                                           error:&e];
        if (!p) {
            printf("pass   %-24s refused: %s\n", c.fragment, e.localizedDescription.UTF8String);
            bad++;
            continue;
        }
        for (int form = 0; form < 3; form++) {
            Source s = source(dev, form, 11 + uint32_t(form));
            if (!s.xr) {
                printf("pass   %-24s %-16s no texture on this Mac\n", c.fragment, form == 2 ? "RGB10A8_2P_XR10" : "?");
                continue;
            }
            const std::vector<uint16_t> metal = draw(dev, q, p, refl, s.xr, other);
            const std::vector<uint16_t> ours = draw(dev, q, p, refl, convert(dev, q, xr, s), other);
            int differ = 0, most = 0;
            for (size_t i = 0; i < metal.size(); i++) {
                const int delta = std::abs(int(metal[i] >> 6) - int(ours[i] >> 6));
                differ += delta != 0;
                most = std::max(most, delta);
                if (delta && getenv("XRORACLE_WHERE"))
                    fprintf(stderr, "  %s %s x %zu y %zu lane %zu: %d %d\n", c.fragment, s.name, (i / 4) % kDstW,
                            i / 4 / kDstW, i % 4, metal[i] >> 6, ours[i] >> 6);
            }
            printf("pass   %-24s %-16s %zu codes, %d differ (largest %d)\n", c.fragment, s.name, metal.size(), differ,
                   most);
            bad += most > 1;
            if (s.surface)
                CVPixelBufferRelease(s.surface);
        }
    }
    return bad;
}

}  // namespace xroracle
