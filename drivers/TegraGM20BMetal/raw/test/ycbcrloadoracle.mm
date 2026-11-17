// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr_load.h against this Mac's Metal, two ways.
//  - The tile: a Load of a two-plane YCbCr code target (8- and 10-bit, 4:2:0 and 4:2:2, both ranges, random planes, a 10-bit
//    word's low six bits random too) read by framebuffer fetch into an RGBA32Float attachment beside it, against
//    ycbcrLoaded; and the planes Metal stores after a pass that loads them and draws a float green through write mask
//    green, against ycbcrStoreOverLoad.
//  - The route: passes that load a two-plane YCbCr target over a CoreVideo surface of random codes and draw floats into
//    scissor rectangles (or draw nothing, or draw beside a second attachment cleared and stored), every byte of both planes.
//    Then the raw path's route on this Mac's GPU: the pack's load state (raw/pack/ycbcr.metal) into an RGBA32Float stand-in,
//    the same draws into it, and its tile-masked store states (raw/pack/ycbcr_store.metal) into the planes through a mask of
//    the tiles the rectangles touch: its bytes must be Metal's, and so must the header's model of both. Metal's passes into
//    a packed target fault on this Mac, so a packed target's route is held to the model alone (its twin's codes, three to a
//    word). `golden` prints the FNV-1a hashes raw_ycbcr_load_test holds the model to.
// usage: ycbcrloadoracle <raw/pack directory> [golden]
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <random>
#include <vector>

#include "../runtime/raw_ycbcr_load.h"
#include "../../test/raw_ycbcr_load_cases.h"

namespace {

const uint16_t kKnots[513] = {
#include "../pack/srgb_knots.h"
};

NSString *const kValues = @"#include <metal_stdlib>\nusing namespace metal;\n"
                          @"struct V { float4 p [[position]]; };\n"
                          @"vertex V strip(uint i [[vertex_id]]) { V v; v.p = float4(float(i & 1) * 2 - 1, "
                          @"float(i >> 1) * 2 - 1, 0, 1); return v; }\n"
                          @"fragment float4 floats(V in [[stage_in]], device const float4 *k [[buffer(0)]], "
                          @"constant uint &w [[buffer(1)]]) { return k[uint(in.p.y) * w + uint(in.p.x)]; }\n";

id<MTLLibrary> packLibrary(id<MTLDevice> dev, NSString *dir, NSString *file)
{
    NSString *metal = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:file]
                                                encoding:NSUTF8StringEncoding
                                                   error:nil];
    NSString *knots = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"srgb_knots.h"]
                                                encoding:NSUTF8StringEncoding
                                                   error:nil];
    if (!metal || !knots)
        return nil;
    metal = [metal stringByReplacingOccurrencesOfString:@"#include \"srgb_knots.h\"" withString:knots];
    MTLCompileOptions *exact = [MTLCompileOptions new];
    exact.mathMode = MTLMathModeSafe;
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:metal options:exact error:&e];
    if (!lib)
        fprintf(stderr, "%s: %s\n", file.UTF8String, e.localizedDescription.UTF8String);
    return lib;
}

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> values, load, store;
    id<MTLBuffer> encode;

    id<MTLRenderPipelineState> state(id<MTLLibrary> from, NSString *vertex, NSString *fragment, MTLPixelFormat f,
                                     MTLPixelFormat second = MTLPixelFormatInvalid)
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [from newFunctionWithName:vertex];
        d.fragmentFunction = [from newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = f;
        d.colorAttachments[1].pixelFormat = second;
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!s)
            fprintf(stderr, "state %s: %s\n", fragment.UTF8String, e.localizedDescription.UTF8String);
        return s;
    }

    id<MTLTexture> texture(MTLPixelFormat f, uint32_t w, uint32_t h, IOSurfaceRef s = nullptr, uint32_t plane = 0)
    {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        return s ? [dev newTextureWithDescriptor:d iosurface:s plane:plane] : [dev newTextureWithDescriptor:d];
    }

    bool run(void (^encode)(id<MTLCommandBuffer>))
    {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        encode(cb);
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted)
            fprintf(stderr, "  command buffer: %s\n", cb.error.localizedDescription.UTF8String);
        return cb.status == MTLCommandBufferStatusCompleted;
    }

    // A pass loading `target` that draws `values` (w texels a row) in `rect`, or nothing, beside a cleared second attachment
    bool drawn(id<MTLTexture> target, MTLLoadAction load, id<MTLRenderPipelineState> s, id<MTLBuffer> values,
               uint32_t w, const MTLScissorRect *rect, bool second)
    {
        id<MTLTexture> other =
            second ? texture(MTLPixelFormatRGBA8Unorm, uint32_t(target.width), uint32_t(target.height)) : nil;
        return run(^(id<MTLCommandBuffer> cb) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = load;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[1].texture = other;
            rp.colorAttachments[1].loadAction = MTLLoadActionClear;
            rp.colorAttachments[1].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            if (rect) {
                uint32_t width = w;
                [e setRenderPipelineState:s];
                [e setScissorRect:*rect];
                [e setFragmentBuffer:values offset:0 atIndex:0];
                [e setFragmentBytes:&width length:4 atIndex:1];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            [e endEncoding];
        });
    }

    // A pass of `s` into `target` over the whole of it, `load` kept where it discards
    bool full(id<MTLTexture> target, id<MTLRenderPipelineState> s, NSArray<id<MTLTexture>> *textures, const void *shape,
              size_t bytes, const raw::YcbcrStoreTiles *tiles, bool srgb)
    {
        return run(^(id<MTLCommandBuffer> cb) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:s];
            for (NSUInteger i = 0; i < textures.count; i++)
                [e setFragmentTexture:textures[i] atIndex:i];
            [e setFragmentBytes:shape length:bytes atIndex:0];
            if (srgb)
                [e setFragmentBuffer:encode offset:0 atIndex:raw::kSrgbEncodeSlot];
            if (tiles)
                [e setFragmentBytes:tiles length:sizeof *tiles atIndex:raw::kYcbcrTilesSlot];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [e endEncoding];
        });
    }
};

raw::YcbcrFacts factsOf(const ycbcrload::Case &c, IOSurfaceRef s)
{
    raw::YcbcrFacts f;
    f.pixelFormat = c.format;
    f.fourcc = IOSurfaceGetPixelFormat(s);
    f.planes = uint32_t(IOSurfaceGetPlaneCount(s));
    f.width = f.surfaceWidth = uint32_t(IOSurfaceGetWidth(s));
    f.height = f.surfaceHeight = uint32_t(IOSurfaceGetHeight(s));
    const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
    for (size_t k = 0; k < 2; k++) {
        f.offset[k] = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, k)) - base;
        f.pitch[k] = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, k));
    }
    f.allocBytes = IOSurfaceGetAllocSize(s);
    f.simple = true;
    f.target = true;
    f.matrix = c.matrix;
    return f;
}

struct Surface {
    CVPixelBufferRef pb = nullptr;
    IOSurfaceRef s = nullptr;
    ycbcrload::Planes saved;

    ycbcrload::Planes read(const ycbcrload::Layout &l) const
    {
        IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
        ycbcrload::Planes p = ycbcrload::readPlanes(l, [&](uint32_t plane) {
            return static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
        });
        IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
        return p;
    }

    void write(const ycbcrload::Layout &l, const ycbcrload::Planes &p) const
    {
        IOSurfaceLock(s, 0, nullptr);
        ycbcrload::writePlanes(l, p, [&](uint32_t plane) {
            return static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
        });
        IOSurfaceUnlock(s, 0, nullptr);
    }
};

Surface makeSurface(const ycbcrload::Case &c, uint32_t w, uint32_t h)
{
    Surface out;
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    if (!CVPixelBufferCreate(nullptr, w, h, c.fourcc, (__bridge CFDictionaryRef)attrs, &out.pb))
        out.s = CVPixelBufferGetIOSurface(out.pb);
    if (out.s && c.matrix)
        IOSurfaceSetValue(out.s, CFSTR("IOSurfaceYCbCrMatrix"), (__bridge CFStringRef) @(c.matrix));
    return out;
}

struct Tally {
    uint64_t cases = 0, rawOff = 0, modelOff = 0, packedOff = 0, failed = 0, unsteady = 0, undefined = 0;
};

// One arm: Metal's pass, the raw route on this GPU and the model, each byte of the planes
void arm(Gpu &g, const ycbcrload::Case &c, const Surface &s, const raw::YcbcrPlan &plan, const ycbcrload::Layout &l,
         const ycbcrload::Arm &a, const std::vector<float> &values, Tally &t, const char *name)
{
    const uint32_t w = l.width, h = l.height;
    const ycbcrload::Planes before = s.saved;
    const ycbcrload::Planes model = ycbcrload::modelPass(c, plan, l, before, values, a, kKnots);
    id<MTLBuffer> valueBuffer = [g.dev newBufferWithBytes:values.data()
                                                   length:values.size() * 4
                                                  options:MTLResourceStorageModeShared];
    const MTLScissorRect rect = {a.x, a.y, a.w, a.h};
    ycbcrload::Planes metal;
    const bool packed = plan.store.packed != 0;
    const ycbcrload::Planes undefined = ycbcrload::tileOrderUndefined(l, plan.store, a);
    for (const std::vector<uint8_t> &plane : undefined)
        t.undefined += uint64_t(std::count(plane.begin(), plane.end(), 1));
    if (!packed) {
        id<MTLTexture> target = g.texture(MTLPixelFormat(c.format), w, h, s.s);
        id<MTLRenderPipelineState> floats = g.state(g.values, @"strip", @"floats", MTLPixelFormat(c.format),
                                                    a.second ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatInvalid);
        // Metal's pass twice over the same planes: an arm whose two runs store different defined bytes is no reference
        ycbcrload::Planes again;
        for (ycbcrload::Planes *into : {&metal, &again}) {
            s.write(l, before);
            if (!g.drawn(target, MTLLoadActionLoad, floats, valueBuffer, w, a.draws ? &rect : nullptr, a.second)) {
                t.failed++;
                return;
            }
            *into = s.read(l);
        }
        if (const uint64_t moved = ycbcrload::bytesOff(metal, again, &undefined)) {
            t.unsteady++;
            printf("  %s %s: Metal stored %llu defined bytes differently on a second run: no reference\n", name,
                   a.label, (unsigned long long)moved);
            return;
        }
    }
    s.write(l, before);
    id<MTLTexture> luma = g.texture(MTLPixelFormat(plan.lumaFormat), plan.lumaWidth, h, s.s, 0);
    id<MTLTexture> chroma = g.texture(MTLPixelFormat(plan.chromaFormat), plan.chromaWidth, plan.chromaHeight, s.s, 1);
    id<MTLTexture> standIn = g.texture(MTLPixelFormatRGBA32Float, w, h);
    NSString *load = [@(raw::ycbcrLoadState(plan.output)) substringFromIndex:10];
    bool ok = g.full(standIn, g.state(g.load, @"ycbcr_vert", load, MTLPixelFormatRGBA32Float), @[ luma, chroma ],
                     plan.shape, sizeof plan.shape, nullptr, false);
    ok = ok && g.drawn(standIn, MTLLoadActionLoad, g.state(g.values, @"strip", @"floats", MTLPixelFormatRGBA32Float),
                       valueBuffer, w, a.draws ? &rect : nullptr, false);
    const std::vector<uint8_t> mask = ycbcrload::tileMask(l, a);
    const raw::YcbcrStoreTiles tiles = {raw::reachTiles(w), raw::reachTiles(h)};
    if (ok && a.draws) {
        id<MTLTexture> reach = g.texture(MTLPixelFormatR8Uint, tiles.x, tiles.y);
        [reach replaceRegion:MTLRegionMake2D(0, 0, tiles.x, tiles.y)
                 mipmapLevel:0
                   withBytes:mask.data()
                 bytesPerRow:tiles.x];
        for (const bool c2 : {false, true}) {
            id<MTLTexture> plane = c2 ? chroma : luma;
            NSMutableArray<id<MTLTexture>> *bound = [NSMutableArray arrayWithObjects:standIn, reach, nil];
            if (packed) {
                id<MTLTexture> kept = g.texture(MTLPixelFormatR32Uint, uint32_t(plane.width), uint32_t(plane.height));
                ok = ok && g.run(^(id<MTLCommandBuffer> cb) {
                    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                    [b copyFromTexture:plane toTexture:kept];
                    [b endEncoding];
                });
                [bound addObject:kept];
            }
            NSString *store = [@(raw::ycbcrMaskedStoreState(c2, plan.store)) substringFromIndex:16];
            ok = ok && g.full(plane, g.state(g.store, @"ycbcr_store_vert", store, plane.pixelFormat), bound,
                              &plan.store, sizeof plan.store, &tiles, plan.store.srgb != 0);
        }
    }
    if (!ok) {
        t.failed++;
        return;
    }
    const ycbcrload::Planes raw = s.read(l);
    t.cases++;
    const uint64_t rawOff = packed ? ycbcrload::bytesOff(raw, model) : ycbcrload::bytesOff(raw, metal, &undefined);
    const uint64_t modelOff = packed ? 0 : ycbcrload::bytesOff(model, metal, &undefined);
    (packed ? t.packedOff : t.rawOff) += rawOff;
    t.modelOff += modelOff;
    if (rawOff || modelOff)
        printf("  %s %s: raw route %llu bytes off %s, model %llu off Metal (%llu bytes moved by Metal)\n", name,
               a.label, (unsigned long long)rawOff, packed ? "the model" : "Metal", (unsigned long long)modelOff,
               (unsigned long long)(packed ? 0 : ycbcrload::bytesOff(metal, before)));
}

NSString *const kFetch = @"#include <metal_stdlib>\nusing namespace metal;\n"
                         @"struct V { float4 p [[position]]; };\n"
                         @"vertex V strip(uint i [[vertex_id]]) { V v; v.p = float4(float(i & 1) * 2 - 1, "
                         @"float(i >> 1) * 2 - 1, 0, 1); return v; }\n"
                         @"struct O { float4 seen [[color(1)]]; };\n"
                         @"fragment O fetch(V in [[stage_in]], half4 c [[color(0)]]) { O o; o.seen = float4(c); "
                         @"return o; }\n"
                         @"fragment float4 green(V in [[stage_in]]) { return float4(0, in.p.x * 0.0123f, 0, 1); }\n";

struct Fetched {
    uint32_t texels = 0, texelsOff = 0, codes = 0, codesOff = 0;
};

// The codes of a code target's planes in `s` (w x h texels, cw x ch chroma pairs): a byte, or a 16-bit word's high ten bits
void codesOf(IOSurfaceRef s, uint32_t bytes, uint32_t w, uint32_t h, uint32_t cw, uint32_t ch,
             std::vector<uint16_t> &luma, std::vector<uint16_t> &chroma)
{
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    for (size_t plane = 0; plane < 2; plane++) {
        const uint8_t *b = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
        const size_t stride = IOSurfaceGetBytesPerRowOfPlane(s, plane);
        const uint32_t codes = plane ? cw * 2 : w, rows = plane ? ch : h;
        std::vector<uint16_t> &out = plane ? chroma : luma;
        out.clear();
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < codes; x++)
                out.push_back(bytes == 2 ? uint16_t((b[y * stride + 2 * x] | b[y * stride + 2 * x + 1] << 8) >> 6)
                                         : b[y * stride + x]);
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
}

// One code target's tile after a Load, and its planes after a Load and a luma-only draw, against the rule
void fetchedTarget(Gpu &g, id<MTLLibrary> lib, uint32_t format, OSType fourcc, bool down, uint32_t bytes,
                   std::mt19937 &rng, Fetched &t)
{
    constexpr uint32_t W = 64, H = 32;
    CVPixelBufferRef pb = nullptr;
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    if (CVPixelBufferCreate(nullptr, W, H, fourcc, (__bridge CFDictionaryRef)attrs, &pb)) {
        printf("no surface %u\n", format);
        t.texelsOff++;
        return;
    }
    IOSurfaceRef s = CVPixelBufferGetIOSurface(pb);
    const raw::YcbcrCodeTarget target = {bytes == 2 ? 1023u : 255u, down};
    const uint32_t cw = W / 2, ch = down ? H / 2 : H;
    IOSurfaceLock(s, 0, nullptr);
    for (size_t plane = 0; plane < 2; plane++) {
        uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
        const size_t stride = IOSurfaceGetBytesPerRowOfPlane(s, plane);
        for (uint32_t y = 0; y < (plane ? ch : H); y++)
            for (uint32_t x = 0; x < (plane ? cw * 2 : W) * bytes; x++)
                b[y * stride + x] = uint8_t(rng());
    }
    IOSurfaceUnlock(s, 0, nullptr);
    std::vector<uint16_t> luma, chroma;
    codesOf(s, bytes, W, H, cw, ch, luma, chroma);
    id<MTLTexture> code = g.texture(MTLPixelFormat(format), W, H, s);
    id<MTLTexture> seen = g.texture(MTLPixelFormatRGBA32Float, W, H);
    id<MTLRenderPipelineState> fetch =
        g.state(lib, @"strip", @"fetch", MTLPixelFormat(format), MTLPixelFormatRGBA32Float);
    const bool fetched = code && fetch && g.run(^(id<MTLCommandBuffer> cb) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = code;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
        rp.colorAttachments[1].texture = seen;
        rp.colorAttachments[1].loadAction = MTLLoadActionClear;
        rp.colorAttachments[1].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:fetch];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
    });
    std::vector<float> got(W * H * 4), want(W * H * 4);
    if (fetched) {
        [seen getBytes:got.data() bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        raw::ycbcrLoaded(target, luma.data(), chroma.data(), W, H, want.data());
        for (uint32_t i = 0; i < W * H; i++)
            t.texelsOff += memcmp(&got[4 * i], &want[4 * i], 16) != 0;
    } else {
        t.texelsOff += W * H;
    }
    t.texels += W * H;
    MTLRenderPipelineDescriptor *masked = [MTLRenderPipelineDescriptor new];
    masked.vertexFunction = [lib newFunctionWithName:@"strip"];
    masked.fragmentFunction = [lib newFunctionWithName:@"green"];
    masked.colorAttachments[0].pixelFormat = MTLPixelFormat(format);
    masked.colorAttachments[0].writeMask = MTLColorWriteMaskGreen;
    id<MTLRenderPipelineState> green = [g.dev newRenderPipelineStateWithDescriptor:masked error:nil];
    const bool drawn = code && green && g.run(^(id<MTLCommandBuffer> cb) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = code;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:green];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
    });
    std::vector<float> greens(W * H);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            greens[y * W + x] = (float(x) + 0.5f) * 0.0123f;
    std::vector<uint16_t> lumaWant(W * H), chromaWant(cw * ch * 2), lumaGot, chromaGot;
    raw::ycbcrStoreOverLoad(target, luma.data(), chroma.data(), greens.data(), W, H, lumaWant.data(),
                            chromaWant.data());
    codesOf(s, bytes, W, H, cw, ch, lumaGot, chromaGot);
    for (size_t i = 0; i < lumaWant.size(); i++)
        t.codesOff += !drawn || lumaGot[i] != lumaWant[i];
    for (size_t i = 0; i < chromaWant.size(); i++)
        t.codesOff += !drawn || chromaGot[i] != chromaWant[i];
    t.codes += uint32_t(lumaWant.size() + chromaWant.size());
    CVPixelBufferRelease(pb);
}

Fetched fetchedLoads(Gpu &g)
{
    Fetched t;
    id<MTLLibrary> lib = [g.dev newLibraryWithSource:kFetch options:nil error:nil];

    struct Config {
        uint32_t format;
        OSType fourcc;
        bool down;
        uint32_t bytes;
    };

    const Config configs[] = {{540, '420v', true, 1},  {540, '420f', true, 1}, {541, '422v', false, 1},
                              {541, '422f', false, 1}, {543, 'x420', true, 2}, {543, 'xf20', true, 2},
                              {544, 'x422', false, 2}, {544, 'xf22', false, 2}};
    std::mt19937 rng(7);
    for (const Config &c : configs)
        for (int seed = 0; seed < 4; seed++)
            fetchedTarget(g, lib, c.format, c.fourcc, c.down, c.bytes, rng, t);
    return t;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: ycbcrloadoracle <raw/pack directory> [golden]\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSError *e = nil;
        g.values = [g.dev newLibraryWithSource:kValues options:nil error:&e];
        g.load = packLibrary(g.dev, @(argv[1]), @"ycbcr.metal");
        g.store = packLibrary(g.dev, @(argv[1]), @"ycbcr_store.metal");
        if (!g.values || !g.load || !g.store)
            return 1;
        uint16_t encode[raw::kYcbcrSrgbSteps + 1];
        raw::ycbcrSrgbEncodeTable(kKnots, encode);
        g.encode = [g.dev newBufferWithBytes:encode length:sizeof encode options:MTLResourceStorageModeShared];
        const bool golden = argc > 2 && !strcmp(argv[2], "golden");
        Tally t;
        for (const ycbcrload::Case &c : golden ? ycbcrload::goldenCases() : ycbcrload::cases())
            for (const ycbcrload::Size &size : ycbcrload::sizes(golden)) {
                Surface s = makeSurface(c, size[0], size[1]);
                raw::YcbcrPlan plan;
                if (!s.s || raw::planYcbcr(factsOf(c, s.s), plan)) {
                    printf("%s: no surface or plan\n", ycbcrload::name(c, size).c_str());
                    continue;
                }
                const ycbcrload::Layout l = ycbcrload::layoutOf(plan, size[0], size[1]);
                s.saved = ycbcrload::randomPlanes(l, c.format * 131 + size[0]);
                const std::vector<float> values = ycbcrload::drawnValues(size[0] * size[1], c.format * 7 + size[1]);
                const std::string name = ycbcrload::name(c, size);
                if (golden) {
                    const ycbcrload::Arm a = ycbcrload::kGoldenArm;
                    arm(g, c, s, plan, l, a, values, t, name.c_str());
                    s.write(l, s.saved);
                    id<MTLTexture> target = g.texture(MTLPixelFormat(c.format), size[0], size[1], s.s);
                    id<MTLBuffer> v = [g.dev newBufferWithBytes:values.data()
                                                         length:values.size() * 4
                                                        options:MTLResourceStorageModeShared];
                    const MTLScissorRect rect = {a.x, a.y, a.w, a.h};
                    if (plan.store.packed || !g.drawn(target, MTLLoadActionLoad,
                                                      g.state(g.values, @"strip", @"floats", MTLPixelFormat(c.format)),
                                                      v, size[0], &rect, false))
                        continue;
                    printf("{%u, '%s', %s, 0x%016llxull},\n", c.format, ycbcrload::fourccText(c.fourcc).c_str(),
                           c.matrix ? ("\"" + std::string(c.matrix) + "\"").c_str() : "nullptr",
                           (unsigned long long)ycbcrload::hash(s.read(l)));
                    continue;
                }
                for (const ycbcrload::Arm &a : ycbcrload::arms(size[0], size[1]))
                    arm(g, c, s, plan, l, a, values, t, name.c_str());
                CVPixelBufferRelease(s.pb);
            }
        printf(
            "%llu passes: raw route %llu bytes off Metal, model %llu off Metal, packed route %llu off the model, %llu "
            "failed, %llu without a steady reference; %llu chroma bytes left out as Metal leaves them undefined (a "
            "block mixing another stored tile's)\n",
            (unsigned long long)t.cases, (unsigned long long)t.rawOff, (unsigned long long)t.modelOff,
            (unsigned long long)t.packedOff, (unsigned long long)t.failed, (unsigned long long)t.unsteady,
            (unsigned long long)t.undefined);
        if (golden)
            return t.rawOff || t.modelOff || t.packedOff || t.failed;
        const Fetched f = fetchedLoads(g);
        printf("%s: %u texels loaded from two-plane YCbCr code targets, %u off the rule; %u codes stored over a load "
               "through write mask green, %u off\n",
               g.dev.name.UTF8String, f.texels, f.texelsOff, f.codes, f.codesOff);
        return t.rawOff || t.modelOff || t.packedOff || t.failed || f.texelsOff || f.codesOff || !f.texels;
    }
}
