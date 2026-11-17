// SPDX-License-Identifier: MIT
// Passes into A1BGR5Unorm, ABGR4Unorm and RGB9E5Float targets (raw/runtime/raw_format_standin.h) against this Mac's Metal,
// every word. The raw path's route on this Mac's GPU: the pass into an RGBA32Float stand-in (cleared to formatStandInClear's
// floats, or filled by the pack's load state), its state made for the stand-in (RGB9E5Float's blends by a destination alpha
// of 1), then the pack's store state into the texture's words as R16Uint or R32Uint. A clear, a loaded texture and draws that
// do not blend over one another store Metal's words; ABGR4 and A1BGR5 blends lie within one code a channel (Metal blends in
// halves); RGB9E5 blends over earlier draws are counted by how far their mantissas lie from Metal's, which truncates each
// draw. A loaded RGB9E5 texture holds its encoder's own words: one of another exponent for its value Metal keeps where no
// draw reaches its tile and the route stores as the encoder's.
// Both texture kinds: one Metal compresses (its clear rounds) and one it does not (ShaderWrite usage: its clear is a draw).
// usage: formatoracle <raw/pack directory>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../runtime/raw_format_standin.h"
#include "../runtime/raw_standin_blend.h"

namespace {

constexpr uint32_t W = 128, H = 64, N = W * H;

NSString *const kDraws = @"#include <metal_stdlib>\nusing namespace metal;\n"
                         @"struct V { float4 p [[position]]; };\n"
                         @"vertex V strip(uint i [[vertex_id]], constant float4 &r [[buffer(0)]]) { V v; v.p = "
                         @"float4(mix(r.x, r.z, float(i & 1)), mix(r.y, r.w, float(i >> 1)), 0, 1); return v; }\n"
                         @"fragment float4 floats(V in [[stage_in]], device const float4 *k [[buffer(0)]]) {\n"
                         @"  return k[uint(in.p.y) * 128 + uint(in.p.x)]; }\n";

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> lib, pack;

    id<MTLRenderPipelineState> state(id<MTLLibrary> from, NSString *v, NSString *f, MTLPixelFormat format,
                                     bool blending = false, bool alphaOne = false)
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [from newFunctionWithName:v];
        d.fragmentFunction = [from newFunctionWithName:f];
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[0];
        a.pixelFormat = format;
        a.blendingEnabled = blending;
        a.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        a.sourceAlphaBlendFactor = MTLBlendFactorOne;
        a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        if (alphaOne) {
            a.sourceRGBBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(a.sourceRGBBlendFactor), true));
            a.destinationRGBBlendFactor =
                MTLBlendFactor(raw::alphaOneFactor(uint32_t(a.destinationRGBBlendFactor), true));
        }
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!s)
            fprintf(stderr, "state %s: %s\n", f.UTF8String, e.localizedDescription.UTF8String);
        return s;
    }

    id<MTLTexture> texture(MTLPixelFormat f, MTLTextureUsage usage)
    {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                     width:W
                                                                                    height:H
                                                                                 mipmapped:NO];
        d.usage = usage;
        d.storageMode = MTLStorageModeShared;
        return [dev newTextureWithDescriptor:d];
    }

    // One pass into `t`: loaded or cleared to `clear`, then each of `draws` over `rect` (clip space) with `s`
    void pass(id<MTLTexture> t, bool load, MTLClearColor clear, id<MTLRenderPipelineState> s,
              const std::vector<id<MTLBuffer>> &draws, const float rect[4], id<MTLTexture> sampled = nil,
              id<MTLTexture> kept = nil, uint32_t loaded = 0)
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = clear;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        if (s) {
            [e setRenderPipelineState:s];
            [e setVertexBytes:rect length:16 atIndex:0];
            if (sampled)
                [e setFragmentTexture:sampled atIndex:0];
            if (kept) {
                [e setFragmentTexture:kept atIndex:1];
                [e setFragmentBytes:&loaded length:4 atIndex:0];
            }
            for (id<MTLBuffer> b : draws) {
                [e setFragmentBuffer:b offset:0 atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            if (!draws.size())
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
};

std::vector<uint32_t> words(id<MTLTexture> t, uint32_t bytes)
{
    std::vector<uint32_t> out(N);
    std::vector<uint8_t> b(N * bytes);
    [t getBytes:b.data() bytesPerRow:W * bytes fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    for (uint32_t i = 0; i < N; i++) {
        out[i] = 0;
        memcpy(&out[i], &b[i * bytes], bytes);
    }
    return out;
}

// The largest difference of a channel's code (a 9-bit mantissa of one exponent for RGB9E5: the words' values over the
// smaller exponent's unit) between two words
uint32_t channelsApart(uint32_t format, uint32_t a, uint32_t b)
{
    if (format == raw::kRgb9e5Float) {
        float x[3], y[3];
        raw::rgb9e5Floats(a, x);
        raw::rgb9e5Floats(b, y);
        const float unit = std::ldexp(1.0f, int(std::min(a >> 27, b >> 27)) - 24);
        float most = 0;
        for (int k = 0; k < 3; k++)
            most = std::fmax(most, std::fabs(x[k] - y[k]) / unit);
        return uint32_t(std::ceil(most));
    }
    const uint32_t shifts[2][4] = {{12, 8, 4, 0}, {11, 6, 1, 0}}, masks[2][4] = {{15, 15, 15, 15}, {31, 31, 31, 1}};
    const int f = format == raw::kA1Bgr5Unorm;
    uint32_t most = 0;
    for (int k = 0; k < 4; k++)
        most = std::max(
            most, uint32_t(std::abs(int(a >> shifts[f][k] & masks[f][k]) - int(b >> shifts[f][k] & masks[f][k]))));
    return most;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: formatoracle <raw/pack directory>\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSError *e = nil;
        g.lib = [g.dev newLibraryWithSource:kDraws options:nil error:&e];
        NSString *src = [NSString stringWithContentsOfFile:[@(argv[1]) stringByAppendingPathComponent:@"fmt.metal"]
                                                  encoding:NSUTF8StringEncoding
                                                     error:nil];
        MTLCompileOptions *exact = [MTLCompileOptions new];
        exact.mathMode = MTLMathModeSafe;
        g.pack = src ? [g.dev newLibraryWithSource:src options:exact error:&e] : nil;
        if (!g.lib || !g.pack) {
            fprintf(stderr, "libraries: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        std::mt19937 rng(9);
        std::uniform_real_distribution<float> u(0, 1);
        const float whole[4] = {-1, -1, 1, 1}, left[4] = {-1, -1, 0.1f, 1};
        uint32_t exactPasses = 0, exactOff = 0, blendPasses = 0, beyondOne = 0;
        for (uint32_t format : {raw::kA1Bgr5Unorm, raw::kAbgr4Unorm, raw::kRgb9e5Float}) {
            const bool shared = format == raw::kRgb9e5Float;
            const uint32_t bytes = shared ? 4 : 2;
            const MTLPixelFormat view = shared ? MTLPixelFormatR32Uint : MTLPixelFormatR16Uint;
            for (const bool compressed : {true, false}) {
                id<MTLTexture> target = g.texture(MTLPixelFormat(format),
                                                  MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
                                                      (compressed ? 0 : MTLTextureUsageShaderWrite)),
                               kept = g.texture(MTLPixelFormatR32Uint, MTLTextureUsageShaderRead),
                               standIn = g.texture(MTLPixelFormatRGBA32Float,
                                                   MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead),
                               stored = g.texture(view, MTLTextureUsageRenderTarget);
                NSString *store = shared                       ? @"fmt_store_rgb9e5_frag"
                                  : format == raw::kAbgr4Unorm ? @"fmt_store_abgr4_frag"
                                                               : @"fmt_store_a1bgr5_frag";
                id<MTLRenderPipelineState> storeState = g.state(g.pack, @"fmt_vert", store, view),
                                           loadState = g.state(g.pack, @"fmt_vert",
                                                               shared ? @"fmt_load_rgb9e5_frag" : @"fmt_load_frag",
                                                               MTLPixelFormatRGBA32Float);
                // case 0 a clear alone, 1 one draw of edge values, 2 a loaded texture and one draw over its left half, 3 three
                // blended draws over a clear
                for (int kase = 0; kase < 4; kase++)
                    for (int round = 0; round < 8; round++) {
                        const bool blended = kase == 3;
                        std::vector<id<MTLBuffer>> draws;
                        for (int n = 0; n < (kase == 0 ? 0 : blended ? 3 : 1); n++) {
                            std::vector<float> v(4 * N);
                            for (size_t i = 0; i < v.size(); i++) {
                                float &x = v[i];
                                const uint32_t k = rng();
                                x = kase == 1 && k % 5 == 0
                                        ? std::vector<float>{NAN, INFINITY, -1, 2, 0.5f, 7e4f}[k / 5 % 6]
                                    : shared ? u(rng) * (k % 3 ? 4.0f : 0.01f)
                                             : u(rng);
                                if (blended && i % 4 == 3)
                                    x = u(rng);
                            }
                            draws.push_back([g.dev newBufferWithBytes:v.data()
                                                               length:v.size() * 4
                                                              options:MTLResourceStorageModeShared]);
                        }
                        const MTLClearColor clear =
                            MTLClearColorMake(u(rng), u(rng), u(rng) * (shared ? 9 : 1), u(rng));
                        std::vector<uint8_t> before(N * bytes);
                        if (kase == 2) {
                            for (uint32_t i = 0; i < N; i++) {
                                uint32_t word = rng();
                                if (shared) {
                                    const float c[3] = {u(rng) * 9, u(rng) * 0.3f, u(rng) * 70};
                                    word = raw::rgb9e5Word(c, true);
                                }
                                memcpy(&before[i * bytes], &word, bytes);
                            }
                            [target replaceRegion:MTLRegionMake2D(0, 0, W, H)
                                      mipmapLevel:0
                                        withBytes:before.data()
                                      bytesPerRow:W * bytes];
                            if (shared)
                                [kept replaceRegion:MTLRegionMake2D(0, 0, W, H)
                                        mipmapLevel:0
                                          withBytes:before.data()
                                        bytesPerRow:W * 4];
                        }
                        id<MTLRenderPipelineState> metal = g.state(g.lib, @"strip", @"floats", MTLPixelFormat(format),
                                                                   blended),
                                                   route =
                                                       g.state(g.lib, @"strip", @"floats", MTLPixelFormatRGBA32Float,
                                                               blended, !raw::formatHoldsAlpha(format));
                        const float *rect = kase == 2 ? left : whole;
                        // the route first: a loaded pass's fill reads the texture before Metal's pass writes it
                        float held[4] = {float(clear.red), float(clear.green), float(clear.blue), float(clear.alpha)};
                        raw::formatStandInClear(format, held, held, compressed);
                        if (kase == 2)
                            g.pass(standIn, false, {}, loadState, {}, whole, shared ? kept : target);
                        g.pass(standIn, kase == 2, MTLClearColorMake(held[0], held[1], held[2], held[3]),
                               kase ? route : nil, draws, rect);
                        g.pass(stored, false, {}, storeState, {}, whole, standIn);
                        g.pass(target, kase == 2, clear, kase ? metal : nil, draws, rect);
                        const std::vector<uint32_t> got = words(target, bytes), routed = words(stored, bytes);
                        uint32_t off = 0, most = 0;
                        for (uint32_t i = 0; i < N; i++) {
                            off += got[i] != routed[i];
                            most = std::max(most, channelsApart(format, got[i], routed[i]));
                        }
                        if (blended) {
                            blendPasses++;
                            beyondOne += !shared && most > 1;
                            if (round == 0)
                                printf("  %u %s blended: %u of %u words off, at most %u %s apart\n", format,
                                       compressed ? "compressed" : "plain", off, N, most,
                                       shared ? "mantissa units" : "codes");
                        } else {
                            exactPasses++;
                            exactOff += off != 0;
                            if (off)
                                printf("  %u %s case %d: %u of %u words off, at most %u apart\n", format,
                                       compressed ? "compressed" : "plain", kase, off, N, most);
                        }
                    }
            }
        }
        printf(
            "%s: %u passes of clears, draws and loads into A1BGR5Unorm, ABGR4Unorm and RGB9E5Float, %u with a word off "
            "Metal's; %u blended passes, %u of ABGR4 or A1BGR5 with a channel more than one code off\n",
            g.dev.name.UTF8String, exactPasses, exactOff, blendPasses, beyondOne);
        return exactOff || beyondOne ? 1 : 0;
    }
}
