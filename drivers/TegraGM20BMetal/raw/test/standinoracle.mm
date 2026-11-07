// SPDX-License-Identifier: MIT
// The frontend's RGB8_444_2P (542) render targets (NativeBridgeMetalFrontend.mm rendersThroughStandIn: an RGBA16Float stand-in,
// loaded from the planes by TwoPlaneStandIn.metal's two_plane_load where the pass loads the texture or stores a DontCare load,
// then two_plane_store into R8Unorm and RG8Unorm views of the planes), run on this Mac's GPU against what this Mac's Metal
// stores for the same passes into a 542 texture over a 4:4:4 two-plane surface ('444f' and '444v'): every byte of both planes
// must be Metal's. The draws write every half in [0, 1], halves past it, infinities and NaNs; the clears a sweep of colors; Load,
// DontCare and Clear passes draw over part of the target.
// The drawn values are halves because this Mac's RGBA16Float target rounds a float fragment toward zero, where Metal's 542 store
// rounds it to the nearest half, ties to even (every float beside every half midpoint: round(h * 255) of that half, h clamped to
// [0, 1], a NaN 0); on the device the stand-in's conversion is GM20B's (NVK's R16G16B16A16_SFLOAT target).
// usage: standinoracle <drivers/NativeMetal/TwoPlaneStandIn.metal>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace {
constexpr int W = 256, H = 256, N = W * H;
constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);

NSString *const kValues =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct V { float4 p [[position]]; };\n"
    @"vertex V cover(uint i [[vertex_id]], constant float4 &box [[buffer(0)]]) {\n"
    @"  float2 c = float2((i << 1) & 2, i & 2); V v; v.p = float4(mix(box.xy, box.zw, c * 0.5) * 2 - 1, 0, 1);"
    @" return v; }\n"
    @"fragment float4 values(V in [[stage_in]], device const float4 *k [[buffer(0)]]) {\n"
    @"  return k[uint(in.p.y) * 256 + uint(in.p.x)]; }\n";

struct Target {
    CVPixelBufferRef pb = nullptr;
    IOSurfaceRef surface = nullptr;
    id<MTLTexture> planes, luma, chroma;
};

Target makeTarget(id<MTLDevice> dev, OSType fourcc)
{
    Target t;
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    if (CVPixelBufferCreate(nullptr, W, H, fourcc, (__bridge CFDictionaryRef)attrs, &t.pb))
        return t;
    t.surface = CVPixelBufferGetIOSurface(t.pb);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kRGB8_444_2P
                                                                                 width:W
                                                                                height:H
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    t.planes = [dev newTextureWithDescriptor:d iosurface:t.surface plane:0];
    d.pixelFormat = MTLPixelFormatR8Unorm;
    d.usage = MTLTextureUsageRenderTarget;
    t.luma = [dev newTextureWithDescriptor:d iosurface:t.surface plane:0];
    d.pixelFormat = MTLPixelFormatRG8Unorm;
    t.chroma = [dev newTextureWithDescriptor:d iosurface:t.surface plane:1];
    return t;
}

void fill(const Target &t, uint8_t seed)
{
    IOSurfaceLock(t.surface, 0, nullptr);
    for (size_t p = 0; p < 2; p++) {
        uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(t.surface, p));
        for (size_t i = 0; i < IOSurfaceGetBytesPerRowOfPlane(t.surface, p) * H; i++)
            b[i] = uint8_t(i * 37 + seed);
    }
    IOSurfaceUnlock(t.surface, 0, nullptr);
}

// the planes' bytes the texture covers: luma, then chroma pairs
std::vector<uint8_t> bytesOf(const Target &t)
{
    std::vector<uint8_t> out;
    IOSurfaceLock(t.surface, kIOSurfaceLockReadOnly, nullptr);
    for (size_t p = 0; p < 2; p++) {
        const uint8_t *b = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(t.surface, p));
        const size_t row = IOSurfaceGetBytesPerRowOfPlane(t.surface, p);
        for (int y = 0; y < H; y++)
            out.insert(out.end(), b + y * row, b + y * row + W * (p + 1));
    }
    IOSurfaceUnlock(t.surface, kIOSurfaceLockReadOnly, nullptr);
    return out;
}

struct Pass {
    MTLLoadAction load;
    MTLClearColor clear;
    const float *values;  // N float4s drawn over `box` (null: no draw)
    float box[4];
};

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLRenderPipelineState> native, intoStandIn, load, store;
    id<MTLTexture> standIn;

    void draw(id<MTLRenderCommandEncoder> e, id<MTLRenderPipelineState> state, const Pass &p)
    {
        if (!p.values)
            return;
        [e setRenderPipelineState:state];
        [e setVertexBytes:p.box length:sizeof p.box atIndex:0];
        [e setFragmentBuffer:[dev newBufferWithBytes:p.values length:N * 16 options:MTLResourceStorageModeShared]
                      offset:0
                     atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }

    // Metal's own pass into the 542 texture
    void nativePass(const Target &t, const Pass &p)
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t.planes;
        rp.colorAttachments[0].loadAction = p.load;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = p.clear;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        draw(e, native, p);
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }

    // the frontend's: the stand-in loaded from the planes (Load), the pass into it, then the store into the planes
    void standInPass(const Target &t, const Pass &p)
    {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        if (p.load != MTLLoadActionClear) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = standIn;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:load];
            [e setFragmentTexture:t.planes atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
        }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = standIn;
        rp.colorAttachments[0].loadAction = p.load;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = p.clear;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        draw(e, intoStandIn, p);
        [e endEncoding];
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t.luma;
        rp.colorAttachments[1].texture = t.chroma;
        for (int i = 0; i < 2; i++) {
            rp.colorAttachments[i].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        }
        e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:store];
        [e setFragmentTexture:standIn atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
};

id<MTLRenderPipelineState> state(id<MTLDevice> dev, id<MTLFunction> v, id<MTLFunction> f,
                                 std::initializer_list<MTLPixelFormat> formats)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = v;
    d.fragmentFunction = f;
    NSUInteger i = 0;
    for (MTLPixelFormat format : formats)
        d.colorAttachments[i++].pixelFormat = format;
    NSError *e = nil;
    id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!s)
        fprintf(stderr, "state: %s\n", e.localizedDescription.UTF8String);
    return s;
}

float bitsOf(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// the specials, every half in [0, 1], then halves past [0, 1] and uniform halves: three channels a texel, in sets of N texels
std::vector<std::vector<float>> valueSets()
{
    std::vector<float> v;
    for (float x :
         {-1.0f, -0.0f, 2.0f, 65504.0f, -65504.0f, INFINITY, -INFINITY, bitsOf(0x7fc00000u), bitsOf(0xffc00000u)})
        v.push_back(x);
    for (uint16_t h = 0; h <= 0x3c00; h++) {
        _Float16 a;
        memcpy(&a, &h, 2);
        v.push_back(float(a));
    }
    std::mt19937 rng(542);
    while (v.size() % (3 * N))
        v.push_back(float(_Float16(std::uniform_real_distribution<float>(-0.25f, 1.25f)(rng))));
    std::vector<std::vector<float>> sets;
    for (size_t at = 0; at < v.size(); at += 3 * N) {
        std::vector<float> k(4 * N);
        for (int i = 0; i < N; i++) {
            for (int c = 0; c < 3; c++)
                k[4 * i + c] = v[at + 3 * i + c];
            k[4 * i + 3] = 1;
        }
        sets.push_back(k);
    }
    return sets;
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: standinoracle <TwoPlaneStandIn.metal>\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSError *e = nil;
        NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
        id<MTLLibrary> standIn = src ? [g.dev newLibraryWithSource:src options:nil error:&e] : nil;
        id<MTLLibrary> values = [g.dev newLibraryWithSource:kValues options:nil error:&e];
        if (!standIn || !values) {
            fprintf(stderr, "libraries: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLFunction> cover = [values newFunctionWithName:@"cover"], draw = [values newFunctionWithName:@"values"];
        id<MTLFunction> full = [standIn newFunctionWithName:@"two_plane_cover"];
        g.native = state(g.dev, cover, draw, {kRGB8_444_2P});
        g.intoStandIn = state(g.dev, cover, draw, {MTLPixelFormatRGBA16Float});
        g.load = state(g.dev, full, [standIn newFunctionWithName : @"two_plane_load"], {
            MTLPixelFormatRGBA16Float
        });
        g.store = state(g.dev, full, [standIn newFunctionWithName : @"two_plane_store"], {
            MTLPixelFormatR8Unorm, MTLPixelFormatRG8Unorm
        });
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                     width:W
                                                                                    height:H
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        g.standIn = [g.dev newTextureWithDescriptor:d];
        if (!g.native || !g.intoStandIn || !g.load || !g.store || !g.standIn)
            return 1;
        std::vector<Pass> passes;
        const std::vector<std::vector<float>> sets = valueSets();
        for (const std::vector<float> &k : sets)
            passes.push_back({MTLLoadActionDontCare, {}, k.data(), {0, 0, 1, 1}});
        for (int i = 0; i <= 64; i++) {
            const double x = i / 60.0 - 0.05;
            passes.push_back({MTLLoadActionClear, MTLClearColorMake(x, 1.1 - x, x * x, 1), nullptr, {}});
        }
        for (MTLLoadAction load : {MTLLoadActionLoad, MTLLoadActionDontCare, MTLLoadActionClear}) {
            passes.push_back({load, MTLClearColorMake(0.3, 0.6, 0.9, 1), sets[0].data(), {0.25f, 0.125f, 0.75f, 0.5f}});
            passes.push_back({load, MTLClearColorMake(0.7, 0.2, 0.4, 1), sets[1].data(), {0.5f, 0, 1, 1}});
        }
        unsigned cases = 0, bad = 0;
        for (OSType fourcc : {(OSType)'444f', (OSType)'444v'}) {
            Target metal = makeTarget(g.dev, fourcc), ours = makeTarget(g.dev, fourcc);
            if (!metal.planes || !ours.luma || !ours.chroma) {
                fprintf(stderr, "no 4:4:4 surface texture\n");
                return 1;
            }
            for (size_t i = 0; i < passes.size(); i++) {
                fill(metal, uint8_t(i));
                fill(ours, uint8_t(i));
                g.nativePass(metal, passes[i]);
                g.standInPass(ours, passes[i]);
                const std::vector<uint8_t> want = bytesOf(metal), got = bytesOf(ours);
                cases++;
                for (size_t b = 0; b < want.size(); b++)
                    if (want[b] != got[b]) {
                        if (bad++ < 8)
                            printf("  %c%c%c%c pass %zu: byte %zu (%s) Metal %u stand-in %u\n", char(fourcc >> 24),
                                   char(fourcc >> 16), char(fourcc >> 8), char(fourcc), i, b,
                                   b < size_t(N) ? "luma" : "chroma", want[b], got[b]);
                        break;
                    }
            }
            CVPixelBufferRelease(metal.pb);
            CVPixelBufferRelease(ours.pb);
        }
        printf("%s: %u passes into RGB8_444_2P, %u with a byte of the planes off\n", g.dev.name.UTF8String, cases, bad);
        return bad ? 1 : 0;
    }
}
