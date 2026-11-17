// SPDX-License-Identifier: MIT
// What an extended-range colour's half shadow changes (raw/runtime/raw_xr_shadow.h), on this Mac's Metal: a pass that writes a
// BGR10_XR or BGRA10_XR attachment and then blends into it, one that clears it to a colour its codes cannot hold and then
// blends, and one that draws a blended state twice, each run whole and again split between its draws (or after its clear) by
// a Store and a Load, which is what a pass reads that holds no shadow. Prints how many words the split changes and the FNV-1a hash of the whole pass's bytes,
// which gpu.raw.xr_shadow (hw_test_app/tests/XrShadowTest.swift) holds the raw path's pass to.
// usage: xrshadoworacle
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kW = 64, kH = 32, kN = kW * kH;

NSString *const kSource = @"#include <metal_stdlib>\nusing namespace metal;\n"
                          @"struct V { float4 p [[position]]; };\n"
                          @"vertex V strip(uint i [[vertex_id]]) { V v; v.p = float4(float(i & 1) * 2 - 1, "
                          @"float(i >> 1) * 2 - 1, 0, 1); return v; }\n"
                          @"fragment float4 texel_values(V in [[stage_in]], device const float4 *k [[buffer(0)]], "
                          @"constant uint &w [[buffer(1)]]) { return k[uint(in.p.y) * w + uint(in.p.x)]; }\n";

uint64_t splitmix(uint64_t &s)
{
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Colours across [-0.75, 1.25], alpha across [0, 1] (XrShadowTest.values)
std::vector<float> values(uint64_t seed)
{
    std::vector<float> v(4 * kN);
    for (uint32_t i = 0; i < 4 * kN; i++) {
        const double u = double(splitmix(seed) >> 11) / double(1ull << 53);
        v[i] = float(i % 4 == 3 ? u : u * 2.0 - 0.75);
    }
    return v;
}

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> lib;

    id<MTLRenderPipelineState> state(MTLPixelFormat f, bool blended)
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"strip"];
        d.fragmentFunction = [lib newFunctionWithName:@"texel_values"];
        d.colorAttachments[0].pixelFormat = f;
        d.colorAttachments[0].blendingEnabled = blended;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        return [dev newRenderPipelineStateWithDescriptor:d error:nil];
    }

    // One pass: `load` (or a clear of `clear`), then each of `draws` (state, values) in order
    void pass(id<MTLTexture> t, bool clear, MTLClearColor color,
              const std::vector<std::pair<id<MTLRenderPipelineState>, const std::vector<float> *>> &draws)
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.colorAttachments[0].clearColor = color;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        const uint32_t w = kW;
        for (const auto &[s, v] : draws) {
            [e setRenderPipelineState:s];
            [e setFragmentBytes:&w length:4 atIndex:1];
            [e setFragmentBuffer:[dev newBufferWithBytes:v->data() length:v->size() * 4 options:0] offset:0 atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
};

uint64_t fnv(const std::vector<uint8_t> &bytes)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : bytes)
        h = (h ^ b) * 0x100000001b3ull;
    return h;
}

}  // namespace

int main()
{
    @autoreleasepool {
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        g.lib = [g.dev newLibraryWithSource:kSource options:nil error:nil];
        const std::vector<float> first = values(1), second = values(2);
        const MTLClearColor off = MTLClearColorMake(0.123, -0.3, 1.1, 0.6);
        for (const auto &[format, name, bytes] : std::initializer_list<std::tuple<uint32_t, const char *, uint32_t>>{
                 {554, "BGR10_XR", 4}, {552, "BGRA10_XR", 8}}) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                         width:kW
                                                                                        height:kH
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModeShared;
            id<MTLRenderPipelineState> plain = g.state(MTLPixelFormat(format), false),
                                       blended = g.state(MTLPixelFormat(format), true);
            // kind 0: a write then a blend; 1: a clear then a blend; 2: one blended state drawn twice
            const auto run = [&](bool split, int kind) {
                id<MTLTexture> t = [g.dev newTextureWithDescriptor:d];
                std::vector<uint8_t> zero(kN * bytes, 0);
                [t replaceRegion:MTLRegionMake2D(0, 0, kW, kH)
                     mipmapLevel:0
                       withBytes:zero.data()
                     bytesPerRow:kW * bytes];
                const std::vector<std::pair<id<MTLRenderPipelineState>, const std::vector<float> *>> draws =
                    kind == 0
                        ? std::vector<std::pair<id<MTLRenderPipelineState>, const std::vector<float> *>>{{plain,
                                                                                                          &first},
                                                                                                         {blended,
                                                                                                          &second}}
                    : kind == 1
                        ? std::vector<std::pair<id<MTLRenderPipelineState>, const std::vector<float> *>>{{blended,
                                                                                                          &second}}
                        : std::vector<std::pair<id<MTLRenderPipelineState>, const std::vector<float> *>>{
                              {blended, &first}, {blended, &second}};
                if (!split) {
                    g.pass(t, kind == 1, off, draws);
                } else if (kind == 1) {
                    g.pass(t, true, off, {});
                    g.pass(t, false, off, draws);
                } else {
                    g.pass(t, false, off, {draws[0]});
                    g.pass(t, false, off, {draws[1]});
                }
                std::vector<uint8_t> out(kN * bytes);
                [t getBytes:out.data() bytesPerRow:kW * bytes fromRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0];
                return out;
            };
            for (const int kind : {0, 1, 2}) {
                const std::vector<uint8_t> whole = run(false, kind), split = run(true, kind);
                uint32_t moved = 0;
                for (uint32_t i = 0; i < kN; i++)
                    moved += memcmp(&whole[i * bytes], &split[i * bytes], bytes) != 0;
                static const char *const kinds[] = {"written then blended", "cleared then blended", "blended twice"};
                printf("%s %s: the split pass moves %u of %u texels; whole pass hash 0x%016llx\n", name, kinds[kind],
                       moved, kN, (unsigned long long)fnv(whole));
            }
        }
        return 0;
    }
}
