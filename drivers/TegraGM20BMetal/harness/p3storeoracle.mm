// SPDX-License-Identifier: MIT
// Stores of render passes into two-plane YCbCr targets under P3_D65 and DCI_P3 (ycbcr.cpp planYcbcr's P3
// shapes), three ways against Metal's own passes on this Mac, every code of both planes: the pack's store states
// (pack/ycbcr_store.metal, plain and tile-masked over every tile) run on this GPU over an RGBA32Float stand-in of the
// pass's texels, and the host model (ycbcr_store.h ycbcrStoreDrawn). Each target is drawn over its whole frame with
// floats (ycbcr_load_cases.h drawnValues, alpha 1 and random), after a DontCare and after a Load of random planes, and
// cleared. A loaded pass's texels no draw reaches are left out: an sRGB 4:2:0 or 4:2:2 target's differ from process to
// process. Metal's passes into packed targets fault, so none is here. `golden` prints the FNV-1a hashes
// ycbcr_load_test holds the model to.
// usage: p3storeoracle <pack directory> [golden]
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <vector>

#include "../test/ycbcr_load_cases.h"
#include "../ycbcr_load.h"
#include "pack_source.h"

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

enum class Pass : uint8_t {
    DontCare,
    Load,
    Clear
};

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> values, store;
    id<MTLBuffer> encode;

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

    // Metal's pass into `target`: `values` (w texels a row) drawn over the whole frame, or a clear to `clear`
    bool metal(id<MTLTexture> target, Pass pass, id<MTLBuffer> values, uint32_t w, MTLClearColor clear)
    {
        id<MTLRenderPipelineState> floats = state(this->values, @"strip", @"floats", target.pixelFormat);
        return floats && run(^(id<MTLCommandBuffer> cb) {
                   MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                   rp.colorAttachments[0].texture = target;
                   rp.colorAttachments[0].loadAction = pass == Pass::Clear  ? MTLLoadActionClear
                                                       : pass == Pass::Load ? MTLLoadActionLoad
                                                                            : MTLLoadActionDontCare;
                   rp.colorAttachments[0].clearColor = clear;
                   rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                   id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                   if (pass != Pass::Clear) {
                       uint32_t width = w;
                       [e setRenderPipelineState:floats];
                       [e setFragmentBuffer:values offset:0 atIndex:0];
                       [e setFragmentBytes:&width length:4 atIndex:1];
                       [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                   }
                   [e endEncoding];
               });
    }

    // A store state `name` (its pack name past "raw.ycbcr_store.") drawn over `plane` from `standIn`, every tile reached
    bool stored(id<MTLTexture> plane, NSString *name, id<MTLTexture> standIn, const gm20b::YcbcrStoreShape &shape)
    {
        id<MTLRenderPipelineState> s = state(store, @"ycbcr_store_vert", name, plane.pixelFormat);
        const gm20b::TileMap tiles =
            gm20b::tileMap({}, gm20b::reachTiles(uint32_t(standIn.width)), gm20b::reachTiles(uint32_t(standIn.height)));
        id<MTLTexture> reach = texture(MTLPixelFormatR8Uint, tiles.tilesX, tiles.tilesY);
        const std::vector<uint8_t> every(size_t(tiles.tilesX) * tiles.tilesY, 1);
        [reach replaceRegion:MTLRegionMake2D(0, 0, tiles.tilesX, tiles.tilesY)
                 mipmapLevel:0
                   withBytes:every.data()
                 bytesPerRow:tiles.tilesX];
        return s && run(^(id<MTLCommandBuffer> cb) {
                   MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                   rp.colorAttachments[0].texture = plane;
                   rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                   rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                   id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                   [e setRenderPipelineState:s];
                   [e setFragmentTexture:standIn atIndex:0];
                   [e setFragmentTexture:reach atIndex:1];
                   [e setFragmentBytes:&shape length:sizeof shape atIndex:0];
                   [e setFragmentBuffer:encode offset:0 atIndex:gm20b::kSrgbEncodeSlot];
                   [e setFragmentBytes:&tiles length:sizeof tiles atIndex:gm20b::kYcbcrTilesSlot];
                   [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                   [e endEncoding];
               });
    }
};

id<MTLLibrary> packLibrary(id<MTLDevice> dev, NSString *dir, NSString *file)
{
    NSString *metal = packSource(dir, file);
    if (!metal)
        return nil;
    MTLCompileOptions *exact = [MTLCompileOptions new];
    exact.mathMode = MTLMathModeSafe;
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:metal options:exact error:&e];
    if (!lib)
        fprintf(stderr, "%s: %s\n", file.UTF8String, e.localizedDescription.UTF8String);
    return lib;
}

struct Surface {
    CVPixelBufferRef pb = nullptr;
    IOSurfaceRef s = nullptr;

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

    ~Surface()
    {
        CVPixelBufferRelease(pb);
    }
};

gm20b::YcbcrFacts factsOf(const ycbcrload::Case &c, IOSurfaceRef s)
{
    gm20b::YcbcrFacts f;
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

// The planes' codes of `stored` (luma, then Cb Cr pairs) in a layout's bytes
ycbcrload::Planes planesOf(const ycbcrload::Layout &l, const std::vector<uint16_t> &luma,
                           const std::vector<uint16_t> &chroma)
{
    ycbcrload::Planes out = {std::vector<uint8_t>(size_t(l.rowBytes[0]) * l.rows[0]),
                             std::vector<uint8_t>(size_t(l.rowBytes[1]) * l.rows[1])};
    for (uint32_t k = 0; k < 2; k++)
        for (size_t i = 0; i < (k ? chroma : luma).size(); i++) {
            const uint16_t c = (k ? chroma : luma)[i];
            if (l.codeBytes == 1) {
                out[k][i] = uint8_t(c);
            } else {
                out[k][2 * i] = uint8_t(c << 6);
                out[k][2 * i + 1] = uint8_t(c >> 2);
            }
        }
    return out;
}

struct Off {
    uint64_t codes = 0, off = 0;
    uint32_t most = 0;

    void add(const ycbcrload::Layout &l, const ycbcrload::Planes &got, const ycbcrload::Planes &want)
    {
        for (uint32_t k = 0; k < 2; k++)
            for (uint32_t y = 0; y < l.rows[k]; y++)
                for (uint32_t x = 0; x < l.codes[k]; x++) {
                    const uint32_t a = ycbcrload::code(l, got[k], k, x, y), b = ycbcrload::code(l, want[k], k, x, y);
                    codes++;
                    off += a != b;
                    most = std::max(most, uint32_t(std::abs(int(a) - int(b))));
                }
    }
};

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: p3storeoracle <pack directory> [golden]\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        g.values = [g.dev newLibraryWithSource:kValues options:nil error:nil];
        g.store = packLibrary(g.dev, @(argv[1]), @"ycbcr_store.metal");
        if (!g.values || !g.store)
            return 1;
        static uint16_t encode[gm20b::kYcbcrSrgbSteps + 1];
        gm20b::ycbcrSrgbEncodeTable(kKnots, encode);
        g.encode = [g.dev newBufferWithBytes:encode length:sizeof encode options:MTLResourceStorageModeShared];
        const bool golden = argc > 2 && !strcmp(argv[2], "golden");
        const std::pair<uint32_t, const char *> targets[] = {
            {520, "420v"}, {520, "420f"}, {522, "422v"}, {522, "422f"}, {525, "x420"}, {525, "xf20"}, {526, "x422"},
            {526, "xf22"}, {503, "444v"}, {503, "444f"}, {507, "x444"}, {507, "xf44"}, {523, "444v"}, {523, "444f"},
            {527, "x444"}, {527, "xf44"}, {500, "420v"}, {502, "422f"}, {505, "x420"}, {506, "xf22"}};
        const uint32_t w = 64, h = 48;
        bool bad = false;
        for (const char *matrix : {"P3_D65", "DCI_P3"})
            for (const auto &[format, text] : targets) {
                const ycbcrload::Case c = {format,
                                           uint32_t(text[0]) << 24 | uint32_t(text[1]) << 16 | uint32_t(text[2]) << 8 |
                                               uint32_t(text[3]),
                                           matrix};
                Surface s;
                NSDictionary *attrs =
                    @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                      (id)kCVPixelBufferMetalCompatibilityKey : @YES};
                if (!CVPixelBufferCreate(nullptr, w, h, c.fourcc, (__bridge CFDictionaryRef)attrs, &s.pb))
                    s.s = CVPixelBufferGetIOSurface(s.pb);
                gm20b::YcbcrPlan plan;
                const char *why = s.s ? nullptr : "no surface";
                if (s.s) {
                    IOSurfaceSetValue(s.s, CFSTR("IOSurfaceYCbCrMatrix"), (__bridge CFStringRef) @(matrix));
                    why = gm20b::planYcbcr(factsOf(c, s.s), plan);
                }
                if (why) {
                    printf("%u %s %s: %s\n", format, text, matrix, why);
                    bad = true;
                    continue;
                }
                const ycbcrload::Layout l = ycbcrload::layoutOf(plan, w, h);
                const ycbcrload::Planes before = ycbcrload::randomPlanes(l, format * 131 + w);
                std::vector<float> values = ycbcrload::drawnValues(w * h, format * 7 + h);
                id<MTLTexture> target = g.texture(MTLPixelFormat(format), w, h, s.s);
                id<MTLTexture> luma = g.texture(MTLPixelFormat(plan.lumaFormat), plan.lumaWidth, h, s.s, 0);
                id<MTLTexture> chroma =
                    g.texture(MTLPixelFormat(plan.chromaFormat), plan.chromaWidth, plan.chromaHeight, s.s, 1);
                id<MTLTexture> standIn = g.texture(MTLPixelFormatRGBA32Float, w, h);
                const MTLClearColor clear = {0.3001, 0.50003, 0.0123, 1};
                Off model, plain, masked;

                const struct {
                    Pass pass;
                    bool alpha;
                } arms[] = {{Pass::DontCare, false}, {Pass::DontCare, true}, {Pass::Load, false}, {Pass::Clear, false}};

                for (const auto &arm : arms) {
                    for (uint32_t i = 0; i < w * h; i++)
                        values[4 * i + 3] = arm.alpha ? float(i % 7) / 6.0f : 1.0f;
                    std::vector<float> texels = values;
                    if (arm.pass == Pass::Clear) {
                        const float k[4] = {float(_Float16(float(clear.red))), float(_Float16(float(clear.green))),
                                            float(_Float16(float(clear.blue))), 1.0f};
                        for (uint32_t i = 0; i < w * h; i++)
                            memcpy(&texels[4 * i], k, sizeof k);
                    }
                    id<MTLBuffer> v = [g.dev newBufferWithBytes:values.data()
                                                         length:values.size() * 4
                                                        options:MTLResourceStorageModeShared];
                    s.write(l, before);
                    if (!g.metal(target, arm.pass, v, w, clear)) {
                        bad = true;
                        continue;
                    }
                    const ycbcrload::Planes metal = s.read(l);
                    std::vector<uint16_t> lumaCodes(size_t(w) * h), chromaCodes(size_t(l.codes[1]) * l.rows[1]);
                    gm20b::ycbcrStoreDrawn(plan.store, encode, texels.data(), w, h, lumaCodes.data(),
                                           chromaCodes.data());
                    model.add(l, planesOf(l, lumaCodes, chromaCodes), metal);
                    [standIn replaceRegion:MTLRegionMake2D(0, 0, w, h)
                               mipmapLevel:0
                                 withBytes:texels.data()
                               bytesPerRow:w * 16];
                    for (const bool tileMasked : {false, true}) {
                        s.write(l, before);
                        bool ok = true;
                        for (const bool c2 : {false, true}) {
                            NSString *name = [@(gm20b::ycbcrMaskedStoreState(c2, plan.store)) substringFromIndex:16];
                            if (!tileMasked)
                                name = [name stringByReplacingOccurrencesOfString:@"_masked" withString:@""];
                            ok = ok && g.stored(c2 ? chroma : luma, name, standIn, plan.store);
                        }
                        if (!ok) {
                            bad = true;
                            continue;
                        }
                        (tileMasked ? masked : plain).add(l, s.read(l), metal);
                    }
                    if (golden && arm.pass == Pass::DontCare && !arm.alpha && plan.store.srgb)
                        printf("{{%u, ycbcrload::fourcc(\"%s\"), \"%s\"}, 0x%016llxull},\n", format, text, matrix,
                               (unsigned long long)ycbcrload::hash(metal));
                }
                bad |= model.off || plain.off || masked.off;
                if (!golden)
                    printf("%u %s %s: %llu codes; model %llu off (max %u), store states %llu off (max %u), tile-masked "
                           "%llu off (max %u)\n",
                           format, text, matrix, (unsigned long long)model.codes, (unsigned long long)model.off,
                           model.most, (unsigned long long)plain.off, plain.most, (unsigned long long)masked.off,
                           masked.most);
            }
        printf("%s\n", bad ? "OFF" : "every code exact");
        return bad;
    }
}
