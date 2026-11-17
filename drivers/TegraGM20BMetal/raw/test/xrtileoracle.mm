// SPDX-License-Identifier: MIT
// raw/runtime/raw_xr_shadow.h's tile value and raw/pack/xr.metal's xr_tile_* states against this Mac's Metal: an attachment of
// every colour code (and alpha code) of BGR10_XR and BGRA10_XR loaded by a pass whose fragment function fetches the tile as
// half into an RGBA32Float attachment, against the tile states drawn on this Mac's GPU from the same bytes and against the
// header's model. The raw path fills an extended-range attachment's half shadow with those states before a pass that holds one.
// usage: xrtileoracle <raw/pack directory>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <vector>

#include "../runtime/raw_xr_shadow.h"

namespace {

constexpr uint32_t kW = 32, kH = 32, kN = kW * kH;

NSString *const kFetch = @"#include <metal_stdlib>\nusing namespace metal;\n"
                         @"struct V { float4 p [[position]]; };\n"
                         @"vertex V strip(uint i [[vertex_id]]) { V v; v.p = float4(float(i & 1) * 2 - 1, "
                         @"float(i >> 1) * 2 - 1, 0, 1); return v; }\n"
                         @"struct F { float4 c1 [[color(1)]]; };\n"
                         @"fragment F fetch(V in [[stage_in]], half4 c0 [[color(0)]]) { F o; o.c1 = float4(c0); "
                         @"return o; }\n";

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> fetch, xr;

    id<MTLTexture> texture(MTLPixelFormat f, MTLTextureUsage usage)
    {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                     width:kW
                                                                                    height:kH
                                                                                 mipmapped:NO];
        d.usage = usage;
        d.storageMode = MTLStorageModeShared;
        return [dev newTextureWithDescriptor:d];
    }

    std::vector<float> floats(id<MTLTexture> t, size_t channelBytes)
    {
        std::vector<float> out(4 * kN);
        if (channelBytes == 4) {
            [t getBytes:out.data() bytesPerRow:kW * 16 fromRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0];
            return out;
        }
        std::vector<_Float16> h(4 * kN);
        [t getBytes:h.data() bytesPerRow:kW * 8 fromRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0];
        for (size_t i = 0; i < h.size(); i++)
            out[i] = float(h[i]);
        return out;
    }

    // Metal's tile of an attachment of `format` holding `bytes`, fetched as half
    std::vector<float> tile(MTLPixelFormat format, const void *bytes, size_t rowBytes)
    {
        id<MTLTexture> codes = texture(format, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        id<MTLTexture> out = texture(MTLPixelFormatRGBA32Float, MTLTextureUsageRenderTarget);
        if (!codes)
            return {};
        [codes replaceRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0 withBytes:bytes bytesPerRow:rowBytes];
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [fetch newFunctionWithName:@"strip"];
        d.fragmentFunction = [fetch newFunctionWithName:@"fetch"];
        d.colorAttachments[0].pixelFormat = format;
        d.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
        d.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA32Float;
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!s) {
            fprintf(stderr, "fetch state: %s\n", e.localizedDescription.UTF8String);
            return {};
        }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = codes;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
        rp.colorAttachments[1].texture = out;
        rp.colorAttachments[1].loadAction = MTLLoadActionClear;
        rp.colorAttachments[1].storeAction = MTLStoreActionStore;
        draw(rp, s, nil);
        return floats(out, 4);
    }

    // The pack's tile state `fragment` drawn from `bytes` viewed as `uintFormat`
    std::vector<float> state(NSString *fragment, MTLPixelFormat uintFormat, const void *bytes, size_t rowBytes)
    {
        id<MTLTexture> codes = texture(uintFormat, MTLTextureUsageShaderRead);
        id<MTLTexture> out = texture(MTLPixelFormatRGBA16Float, MTLTextureUsageRenderTarget);
        [codes replaceRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0 withBytes:bytes bytesPerRow:rowBytes];
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [xr newFunctionWithName:@"xr_vert"];
        d.fragmentFunction = [xr newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!s) {
            fprintf(stderr, "%s: %s\n", fragment.UTF8String, e.localizedDescription.UTF8String);
            return {};
        }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = out;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        draw(rp, s, codes);
        return floats(out, 2);
    }

    void draw(MTLRenderPassDescriptor *rp, id<MTLRenderPipelineState> s, id<MTLTexture> sampled)
    {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:s];
        [e setFragmentTexture:sampled atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
};

bool same(float a, float b)
{
    return a == b || (a != a && b != b);
}

uint32_t compare(const char *name, raw::XrForm form, const std::vector<float> &metal, const std::vector<float> &state,
                 const uint32_t codes[][4])
{
    uint32_t stateOff = 0, modelOff = 0, shown = 0;
    for (uint32_t i = 0; i < kN && !metal.empty() && !state.empty(); i++)
        for (uint32_t c = 0; c < 4; c++) {
            const float model = raw::xrTileValue(form, codes[i][c], c == 3);
            stateOff += !same(state[4 * i + c], metal[4 * i + c]);
            modelOff += !same(model, metal[4 * i + c]);
            if ((!same(state[4 * i + c], metal[4 * i + c]) || !same(model, metal[4 * i + c])) && shown++ < 4)
                printf("  %s code %u channel %u: Metal %.9g, state %.9g, model %.9g\n", name, codes[i][c], c,
                       metal[4 * i + c], state[4 * i + c], model);
        }
    printf("%s: %u channels, tile state %u off Metal's tile, model %u off%s\n", name, 4 * kN, stateOff, modelOff,
           metal.empty() || state.empty() ? " (not run)" : "");
    return stateOff + modelOff + (metal.empty() || state.empty());
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: xrtileoracle <raw/pack directory>\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSError *e = nil;
        g.fetch = [g.dev newLibraryWithSource:kFetch options:nil error:&e];
        NSString *src = [NSString stringWithContentsOfFile:[@(argv[1]) stringByAppendingPathComponent:@"xr.metal"]
                                                  encoding:NSUTF8StringEncoding
                                                     error:nil];
        g.xr = src ? [g.dev newLibraryWithSource:src options:nil error:&e] : nil;
        if (!g.fetch || !g.xr) {
            fprintf(stderr, "libraries: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        static uint32_t codes[kN][4];
        std::vector<uint32_t> bgr(kN);
        std::vector<uint16_t> bgra(4 * kN);
        for (uint32_t i = 0; i < kN; i++) {
            const uint32_t r = i, gc = (i + 341) % 1024, b = (i + 682) % 1024, a = (i * 7) % 1024;
            codes[i][0] = r;
            codes[i][1] = gc;
            codes[i][2] = b;
            codes[i][3] = a;
            bgr[i] = 0xC0000000u | r << 20 | gc << 10 | b;
            bgra[4 * i + 0] = uint16_t(b << 6);
            bgra[4 * i + 1] = uint16_t(gc << 6);
            bgra[4 * i + 2] = uint16_t(r << 6);
            bgra[4 * i + 3] = uint16_t(a << 6);
        }
        uint32_t bad = 0;
        bad += compare("BGR10_XR", raw::XrForm::Bgr10, g.tile(MTLPixelFormat(554), bgr.data(), kW * 4),
                       g.state(@"xr_tile_bgr10_frag", MTLPixelFormatR32Uint, bgr.data(), kW * 4), codes);
        bad += compare("BGRA10_XR", raw::XrForm::Bgra10, g.tile(MTLPixelFormat(552), bgra.data(), kW * 8),
                       g.state(@"xr_tile_bgra10_frag", MTLPixelFormatRGBA16Uint, bgra.data(), kW * 8), codes);
        return bad != 0;
    }
}
