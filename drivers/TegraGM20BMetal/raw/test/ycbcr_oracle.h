// SPDX-License-Identifier: MIT
// The two-plane YCbCr targets the store oracles render (ycbcrstoreoracle.mm, ycbcrblendoracle.mm): CoreVideo surfaces of
// 128x64, their planes' codes, the store rule's codes of a pass's texels (raw_ycbcr_store.h), and the raw path's route on this
// Mac's GPU: an RGBA32Float stand-in of the pass's texels, the pack's store states (raw/pack/ycbcr_store.metal) into the
// planes.
#pragma once
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <vector>

#include "../runtime/raw_ycbcr_store.h"

namespace ycbcr_oracle {
constexpr uint32_t W = 128, H = 64, N = W * H;

inline const uint16_t kKnots[513] = {
#include "../pack/srgb_knots.h"
};

struct Config {
    uint32_t format;
    OSType fourcc;
    const char *matrix;
};

inline bool fullRange(OSType f)
{
    return f == '420f' || f == '422f' || f == '444f' || f == 'xf20' || f == 'xf22' || f == 'xf44';
}

// The target's subsampling: its 4:2:0, 4:2:2 or 4:4:4 surface's
inline void halved(OSType f, bool &across, bool &down)
{
    across = f != '444v' && f != '444f' && f != 'x444' && f != 'xf44';
    down = f == '420v' || f == '420f' || f == 'x420' || f == 'xf20';
}

inline raw::YcbcrConvertedTarget convertedTarget(const Config &c)
{
    raw::YcbcrConvertedTarget t;
    t.bits = c.format < 505 || (c.format >= 520 && c.format < 525) ? 8 : 10;
    t.srgb = c.format >= 520;
    t.full = fullRange(c.fourcc);
    t.matrix = !c.matrix                          ? raw::YcbcrMatrix::Bt601
               : !strcmp(c.matrix, "ITU_R_709_2") ? raw::YcbcrMatrix::Bt709
                                                  : raw::YcbcrMatrix::Bt2020;
    halved(c.fourcc, t.across, t.down);
    return t;
}

// The format a target stores as: under P3_D65 or DCI_P3 a converted one's codes format (raw_ycbcr.cpp matrixIndex)
inline uint32_t storedAs(const Config &c)
{
    if (!c.matrix || (strcmp(c.matrix, "P3_D65") && strcmp(c.matrix, "DCI_P3")))
        return c.format;
    switch (c.format) {
    case 500:
        return 540;
    case 502:
        return 541;
    case 505:
        return 543;
    case 506:
        return 544;
    default:
        return c.format;
    }
}

inline raw::YcbcrStoreShape shapeOf(const Config &c)
{
    raw::YcbcrCodeTarget codes;
    if (!raw::ycbcrCodeTarget(storedAs(c), codes))
        return raw::ycbcrConvertedShape(convertedTarget(c));
    return raw::ycbcrCodesShape(codes.max > 255 ? 10 : 8, true, codes.halvedDown);
}

struct Surface {
    CVPixelBufferRef pb = nullptr;
    IOSurfaceRef s = nullptr;
};

inline Surface makeSurface(const Config &c)
{
    Surface out;
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    if (!CVPixelBufferCreate(nullptr, W, H, c.fourcc, (__bridge CFDictionaryRef)attrs, &out.pb))
        out.s = CVPixelBufferGetIOSurface(out.pb);
    if (out.s && c.matrix)
        IOSurfaceSetValue(out.s, CFSTR("IOSurfaceYCbCrMatrix"), (__bridge CFStringRef) @(c.matrix));
    return out;
}

// Plane 0's codes then plane 1's, `bytes` a code (a 10-bit code its word's high bits)
inline std::vector<uint16_t> planes(IOSurfaceRef s, uint32_t bytes)
{
    std::vector<uint16_t> out;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    for (size_t p = 0; p < 2; p++) {
        const uint8_t *b = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, p));
        const size_t codes = p ? 2 * IOSurfaceGetWidthOfPlane(s, p) : IOSurfaceGetWidthOfPlane(s, p);
        for (size_t y = 0; y < IOSurfaceGetHeightOfPlane(s, p); y++)
            for (size_t x = 0; x < codes; x++) {
                const uint8_t *at = b + y * IOSurfaceGetBytesPerRowOfPlane(s, p) + x * bytes;
                out.push_back(bytes == 2 ? uint16_t((at[0] | at[1] << 8) >> 6) : at[0]);
            }
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return out;
}

inline void scribble(IOSurfaceRef s, uint32_t seed)
{
    IOSurfaceLock(s, 0, nullptr);
    for (size_t p = 0; p < 2; p++) {
        uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, p));
        for (size_t i = 0; i < IOSurfaceGetBytesPerRowOfPlane(s, p) * IOSurfaceGetHeightOfPlane(s, p); i++)
            b[i] = uint8_t(i * 31 + seed);
    }
    IOSurfaceUnlock(s, 0, nullptr);
}

inline std::vector<uint16_t> reference(const Config &c, const uint16_t *encode, const std::vector<float> &v,
                                       raw::YcbcrTexelSource source)
{
    bool across, down;
    halved(c.fourcc, across, down);
    std::vector<uint16_t> luma(N), chroma(2 * (across ? W / 2 : W) * (down ? H / 2 : H));
    raw::YcbcrCodeTarget t;
    if (raw::ycbcrCodeTarget(storedAs(c), t))
        raw::ycbcrStore(t, v.data(), W, H, source, luma.data(), chroma.data());
    else
        raw::ycbcrStoreConverted(convertedTarget(c), encode, v.data(), W, H, source, luma.data(), chroma.data());
    luma.insert(luma.end(), chroma.begin(), chroma.end());
    return luma;
}

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> lib, store;
    id<MTLBuffer> encode;  // pack/srgb_encode.h, at the _srgb states' buffer(1) as the device binds it

    id<MTLRenderPipelineState> state(id<MTLLibrary> from, NSString *vertex, NSString *fragment, MTLPixelFormat f)
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [from newFunctionWithName:vertex];
        d.fragmentFunction = [from newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = f;
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!s)
            fprintf(stderr, "state %s: %s\n", fragment.UTF8String, e.localizedDescription.UTF8String);
        return s;
    }

    // One pass into `target`: a clear of `clear` (no state) or a draw with `s` of `values` or the `sampled` texture
    void pass(id<MTLTexture> target, id<MTLRenderPipelineState> s, const float *values, MTLClearColor clear,
              id<MTLTexture> sampled = nil, const raw::YcbcrStoreShape *shape = nullptr)
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = s ? MTLLoadActionDontCare : MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = clear;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        if (s) {
            [e setRenderPipelineState:s];
            if (values)
                [e setFragmentBuffer:[dev newBufferWithBytes:values length:N * 16 options:MTLResourceStorageModeShared]
                              offset:0
                             atIndex:0];
            if (sampled)
                [e setFragmentTexture:sampled atIndex:0];
            if (shape)
                [e setFragmentBytes:shape length:sizeof *shape atIndex:0];
            if (shape && shape->srgb)
                [e setFragmentBuffer:encode offset:0 atIndex:raw::kSrgbEncodeSlot];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
};

// The pack's store states' bytes of `standIn`, an RGBA32Float texture of the pass's texels: plane 0's codes then plane 1's
inline std::vector<uint16_t> routeOf(Gpu &g, const Config &c, id<MTLTexture> standIn)
{
    const raw::YcbcrStoreShape shape = shapeOf(c);
    const bool wide = shape.max > 255;
    const uint32_t bytes = wide ? 2 : 1, cw = shape.across ? W / 2 : W, ch = shape.down ? H / 2 : H;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                  width:W
                                                                                 height:H
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    NSString *suffix = shape.srgb ? @"_srgb_frag" : @"_frag";
    std::vector<uint16_t> out;
    for (int plane = 0; plane < 2; plane++) {
        const MTLPixelFormat f = plane ? (wide ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm)
                                       : (wide ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm);
        td.pixelFormat = f;
        td.width = plane ? cw : W;
        td.height = plane ? ch : H;
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> t = [g.dev newTextureWithDescriptor:td];
        NSString *name =
            [NSString stringWithFormat:@"ycbcr_store_%@%u%@", plane ? @"chroma" : @"luma", wide ? 16 : 8, suffix];
        g.pass(t, g.state(g.store, @"ycbcr_store_vert", name, f), nullptr, {}, standIn, &shape);
        const uint32_t codes = uint32_t(td.width) * (plane ? 2 : 1), rows = uint32_t(td.height);
        std::vector<uint8_t> b(codes * bytes * rows);
        [t getBytes:b.data() bytesPerRow:codes * bytes fromRegion:MTLRegionMake2D(0, 0, td.width, rows) mipmapLevel:0];
        for (uint32_t i = 0; i < codes * rows; i++)
            out.push_back(wide ? uint16_t((b[2 * i] | b[2 * i + 1] << 8) >> 6) : b[i]);
    }
    return out;
}

// The same of `texels` (the stand-in's)
inline std::vector<uint16_t> route(Gpu &g, const Config &c, const std::vector<float> &texels)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                  width:W
                                                                                 height:H
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> standIn = [g.dev newTextureWithDescriptor:td];
    [standIn replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:texels.data() bytesPerRow:W * 16];
    return routeOf(g, c, standIn);
}

}  // namespace ycbcr_oracle
