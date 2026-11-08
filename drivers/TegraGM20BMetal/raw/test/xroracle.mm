// SPDX-License-Identifier: MIT
// raw GM20B path: the extended-range formats' arithmetic (runtime/raw_xr.h, pack/xr.metal), run on this Mac's own GPU against what
// this Mac's Metal reads and stores:
//  - decode: every code of BGR10_XR and BGRA10_XR (alpha codes included) and of RGB10A8_2P_XR10's color word and alpha byte, read
//    by Metal as half, against pack/xr.metal's stand-in of the same bytes;
//  - clears: Metal's Clear of each form for colors around every boundary, in a pass with a draw call and in one without (Metal's
//    straight route on a texture it keeps compressed), against raw_xr.h xrClearTexel;
//  - Core Animation's extended-range passes (QuartzCore's own functions): each drawn into its extended-range target with its
//    source sampled natively, and again with the source's stand-in sampled instead, as the raw path draws it: the codes each
//    stores, compared.
// usage: xroracle <pack/xr.metal> <QuartzCore default.metallib>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "../runtime/raw_xr.h"
#include "xroracle_passes.h"

namespace {
constexpr MTLPixelFormat kPlanes = MTLPixelFormat(550);

NSString *const kReaders =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"kernel void read_half(texture2d<half> t [[texture(0)]], device half4 *out [[buffer(0)]], uint2 at "
    @"[[thread_position_in_grid]]) {\n"
    @"  constexpr sampler s(filter::nearest, coord::pixel);\n"
    @"  if (at.x < t.get_width() && at.y < t.get_height()) out[at.y * t.get_width() + at.x] = t.sample(s, float2(at) + "
    @"0.5);\n}\n"
    @"struct V { float4 p [[position]]; };\n"
    @"vertex V nothing_vert(uint v [[vertex_id]]) { V o; o.p = float4(2.0, 2.0, 0.0, 1.0); return o; }\n"
    @"fragment half4 nothing_frag(V in [[stage_in]]) { return half4(0.0h); }\n";

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> readers, xr;
};

void run(Gpu &g, void (^encode)(id<MTLCommandBuffer>))
{
    id<MTLCommandBuffer> cb = [g.q commandBuffer];
    encode(cb);
    [cb commit];
    [cb waitUntilCompleted];
}

id<MTLTexture> texture(Gpu &g, MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage usage,
                       MTLStorageMode mode = MTLStorageModeShared)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = usage;
    d.storageMode = mode;
    return [g.dev newTextureWithDescriptor:d];
}

// Metal's half reads of `t`, every texel
std::vector<uint16_t> metalReads(Gpu &g, id<MTLTexture> t)
{
    id<MTLComputePipelineState> p =
        [g.dev newComputePipelineStateWithFunction:[g.readers newFunctionWithName:@"read_half"] error:nil];
    const NSUInteger n = t.width * t.height;
    id<MTLBuffer> out = [g.dev newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
    run(g, ^(id<MTLCommandBuffer> cb) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:p];
        [e setTexture:t atIndex:0];
        [e setBuffer:out offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)];
        [e endEncoding];
    });
    const uint16_t *h = (const uint16_t *)out.contents;
    return std::vector<uint16_t>(h, h + n * 4);
}

// pack/xr.metal's stand-in of codes held in `codes` (its integer format), as the raw path draws it
std::vector<uint16_t> standIn(Gpu &g, id<MTLTexture> codes, const char *fragment)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g.xr newFunctionWithName:@"xr_vert"];
    d.fragmentFunction = [g.xr newFunctionWithName:@(fragment)];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [g.dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) {
        fprintf(stderr, "%s: %s\n", fragment, e.localizedDescription.UTF8String);
        exit(1);
    }
    id<MTLTexture> out = texture(g, MTLPixelFormatRGBA16Float, codes.width, codes.height,
                                 MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    run(g, ^(id<MTLCommandBuffer> cb) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = out;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:pass];
        [r setRenderPipelineState:p];
        [r setFragmentTexture:codes atIndex:0];
        [r drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [r endEncoding];
    });
    std::vector<uint16_t> h(codes.width * codes.height * 4);
    [out getBytes:h.data()
        bytesPerRow:codes.width * 8
         fromRegion:MTLRegionMake2D(0, 0, codes.width, codes.height)
        mipmapLevel:0];
    return h;
}

int compare(const char *what, const std::vector<uint16_t> &metal, const std::vector<uint16_t> &ours)
{
    int bad = 0;
    for (size_t i = 0; i < metal.size(); i++)
        if (metal[i] != ours[i] && bad++ < 4)
            fprintf(stderr, "  %s: texel %zu lane %zu: Metal %04x, stand-in %04x\n", what, i / 4, i % 4, metal[i],
                    ours[i]);
    printf("decode %-16s %zu lanes, %d differ\n", what, metal.size(), bad);
    return bad;
}

// Every code in every lane: texel i holds code i in red, a permutation of it in green and blue, and for BGRA10_XR alpha code i
int decodes(Gpu &g)
{
    const NSUInteger w = 1024;
    std::vector<uint32_t> bgr(w);
    std::vector<uint16_t> bgra(w * 4);
    for (uint32_t i = 0; i < w; i++) {
        const uint32_t r = i, gr = (i * 7 + 3) % 1024, b = (i * 13 + 5) % 1024;
        bgr[i] = 0xC0000000u | r << 20 | gr << 10 | b;
        bgra[i * 4] = uint16_t(b << 6);
        bgra[i * 4 + 1] = uint16_t(gr << 6);
        bgra[i * 4 + 2] = uint16_t(r << 6);
        bgra[i * 4 + 3] = uint16_t(i << 6);
    }
    int bad = 0;
    {
        id<MTLTexture> xr = texture(g, MTLPixelFormatBGR10_XR, w, 1, MTLTextureUsageShaderRead);
        id<MTLTexture> codes = texture(g, MTLPixelFormatR32Uint, w, 1, MTLTextureUsageShaderRead);
        for (id<MTLTexture> t : @[ xr, codes ])
            [t replaceRegion:MTLRegionMake2D(0, 0, w, 1) mipmapLevel:0 withBytes:bgr.data() bytesPerRow:w * 4];
        bad += compare("BGR10_XR", metalReads(g, xr), standIn(g, codes, "xr_bgr10_frag"));
    }
    {
        id<MTLTexture> xr = texture(g, MTLPixelFormatBGRA10_XR, w, 1, MTLTextureUsageShaderRead);
        id<MTLTexture> codes = texture(g, MTLPixelFormatRGBA16Uint, w, 1, MTLTextureUsageShaderRead);
        for (id<MTLTexture> t : @[ xr, codes ])
            [t replaceRegion:MTLRegionMake2D(0, 0, w, 1) mipmapLevel:0 withBytes:bgra.data() bytesPerRow:w * 8];
        bad += compare("BGRA10_XR", metalReads(g, xr), standIn(g, codes, "xr_bgra10_frag"));
    }
    // RGB10A8_2P_XR10: Metal makes it over a 'b3a8' surface's plane 0 only (CoreVideo's two planes: the color words, the alpha
    // bytes); the raw path holds the pair side by side (RG32Uint)
    {
        CVPixelBufferRef pb = nullptr;
        NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
        if (CVPixelBufferCreate(nullptr, w, 1, 'b3a8', (__bridge CFDictionaryRef)attributes, &pb)) {
            printf("decode RGB10A8_2P_XR10: no 'b3a8' surface on this Mac\n");
            return bad;
        }
        CVPixelBufferLockBaseAddress(pb, 0);
        auto *words = (uint32_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        auto *alpha = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        std::vector<uint32_t> pairs(w * 2);
        for (uint32_t i = 0; i < w; i++) {
            words[i] = bgr[i] & 0x3fffffffu;
            alpha[i] = uint8_t(i);
            pairs[i * 2] = words[i];
            pairs[i * 2 + 1] = i & 0xff;
        }
        CVPixelBufferUnlockBaseAddress(pb, 0);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPlanes
                                                                                     width:w
                                                                                    height:1
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> xr = [g.dev newTextureWithDescriptor:d iosurface:CVPixelBufferGetIOSurface(pb) plane:0];
        id<MTLTexture> codes = texture(g, MTLPixelFormatRG32Uint, w, 1, MTLTextureUsageShaderRead);
        [codes replaceRegion:MTLRegionMake2D(0, 0, w, 1) mipmapLevel:0 withBytes:pairs.data() bytesPerRow:w * 8];
        if (!xr)
            printf("decode RGB10A8_2P_XR10: Metal made no texture over the surface\n");
        else
            bad += compare("RGB10A8_2P_XR10", metalReads(g, xr), standIn(g, codes, "xr_planes_frag"));
        CVPixelBufferRelease(pb);
    }
    return bad;
}

// The clear colors: each channel's value near every code boundary of a grid, the ends, past them, and NaN
std::vector<std::array<float, 4>> clearColors()
{
    std::vector<std::array<float, 4>> out;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> any(-1.0f, 1.6f);
    const float nan = std::nanf("");
    const float fixed[] = {0.0f, -0.0f, 1.0f, 0.5f, 0.25f, -0.25f, -0.752941f, 1.25098f, 1.5f, 2.0f, -2.0f, 1e-5f, nan};
    for (float v : fixed)
        out.push_back({v, -v, v * 0.5f, v});
    for (int c = 0; c < 1024; c += 3) {
        const float edge = (float(c) - 384.0f + 0.5f) / 510.0f;
        out.push_back({edge, std::nextafter(edge, 9.0f), std::nextafter(edge, -9.0f), edge});
    }
    for (int i = 0; i < 400; i++)
        out.push_back({any(rng), any(rng), any(rng), any(rng)});
    return out;
}

// Metal's texels of a 16x16 target of `f` cleared to each color, read back as the integer target's components
int clears(Gpu &g, MTLPixelFormat f, bool drawn)
{
    const raw::XrForm form = raw::xrForm(uint32_t(f));
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [g.readers newFunctionWithName:@"nothing_vert"];
    pd.fragmentFunction = [g.readers newFunctionWithName:@"nothing_frag"];
    pd.colorAttachments[0].pixelFormat = f;
    id<MTLRenderPipelineState> nothing = [g.dev newRenderPipelineStateWithDescriptor:pd error:nil];
    const NSUInteger bpp = form == raw::XrForm::Bgr10 ? 4 : 8;
    int bad = 0, n = 0;
    for (const auto &color : clearColors()) {
        id<MTLTexture> t =
            texture(g, f, 16, 16, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
        id<MTLBuffer> out = [g.dev newBufferWithLength:16 * 16 * bpp options:MTLResourceStorageModeShared];
        run(g, ^(id<MTLCommandBuffer> cb) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = t;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);
            id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:pass];
            if (drawn) {
                [r setRenderPipelineState:nothing];
                [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [r endEncoding];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:t
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(16, 16, 1)
                                toBuffer:out
                       destinationOffset:0
                  destinationBytesPerRow:16 * bpp
                destinationBytesPerImage:16 * 16 * bpp];
            [b endEncoding];
        });
        uint32_t want[4];
        raw::xrClearTexel(form, color.data(), !drawn, want);
        uint32_t got[4] = {};
        const uint8_t *p = (const uint8_t *)out.contents + (5 * 16 + 9) * bpp;
        if (form == raw::XrForm::Bgr10) {
            memcpy(got, p, 4);
        } else {
            for (int k = 0; k < 4; k++)
                got[k] = uint32_t(p[2 * k] | p[2 * k + 1] << 8);
        }
        n++;
        if (memcmp(got, want, sizeof got) && bad++ < 4)
            fprintf(stderr, "  clear %lu %s (%g %g %g %g): Metal %08x %08x %08x %08x, raw %08x %08x %08x %08x\n",
                    (unsigned long)f, drawn ? "drawn" : "straight", color[0], color[1], color[2], color[3], got[0],
                    got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
    }
    printf("clear  %-16s %-9s %d colors, %d differ\n", f == MTLPixelFormatBGR10_XR ? "BGR10_XR" : "BGRA10_XR",
           drawn ? "drawn" : "straight", n, bad);
    return bad;
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: xroracle <pack/xr.metal> <QuartzCore default.metallib>\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSError *e = nil;
        g.readers = [g.dev newLibraryWithSource:kReaders options:nil error:&e];
        NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
        g.xr = src ? [g.dev newLibraryWithSource:src options:nil error:&e] : nil;
        id<MTLLibrary> qc = [g.dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&e];
        if (!g.readers || !g.xr || !qc) {
            fprintf(stderr, "libraries: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        printf("%s\n", g.dev.name.UTF8String);
        int bad = decodes(g);
        for (MTLPixelFormat f : {MTLPixelFormatBGR10_XR, MTLPixelFormatBGRA10_XR})
            for (bool drawn : {true, false})
                bad += clears(g, f, drawn);
        bad += xroracle::passes(g.dev, g.q, qc, g.xr);
        printf("%s\n", bad ? "FAIL" : "ok");
        return bad ? 1 : 0;
    }
}
