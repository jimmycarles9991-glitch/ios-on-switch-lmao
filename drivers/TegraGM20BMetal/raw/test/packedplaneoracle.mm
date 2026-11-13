// SPDX-License-Identifier: MIT
// raw GM20B path: R10Unorm_PACKED and RG10Unorm_PACKED (runtime/raw_packed_plane.h) on this Mac's own GPU, over CoreVideo
// 'p420', 'pf20', 'p422', 'pf22', 'p444' and 'pf44' surfaces of odd and even widths:
//  - reads: random words (bits 30-31 set or not) read through Metal's own textures of the two formats over planes 0 and 1,
//    against the pack's ycbcr_packed_plane_frag drawn from the same words into RGB10A2Unorm, as planPackedPlane plans it,
//    and against packedPlaneTexel's codes: every texel's channels must be Metal's;
//  - stores: the pack's ycbcr_store_plane_packed_frag drawn from stand-ins of floats beside the half midpoints and the
//    1/1023 ties into R32Uint, against packedPlaneDrawn; then those words in the surface, read through Metal's textures:
//    every texel must read the code the 10-bit rule holds (raw_ycbcr_store_rule.h ycbcrTexelCode).
// Metal's own passes into the two formats are not drawn: this Mac's GPU faults on them (raw_packed_plane.h).
// usage: packedplaneoracle <raw/pack directory>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <random>
#include <vector>

#include "../runtime/raw_packed_plane.h"

namespace {

NSString *const kReader =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"kernel void read_texels(texture2d<float, access::read> t [[texture(0)]], device float4 *out "
    @"[[buffer(0)]], uint2 at [[thread_position_in_grid]]) {\n"
    @"  if (at.x < t.get_width() && at.y < t.get_height()) out[at.y * t.get_width() + at.x] = t.read(at);\n}\n";

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> conversion, store, reader;

    id<MTLRenderPipelineState> state(id<MTLLibrary> lib, NSString *vertex, NSString *fragment, MTLPixelFormat f)
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:vertex];
        d.fragmentFunction = [lib newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = f;
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!s)
            fprintf(stderr, "%s: %s\n", fragment.UTF8String, e.localizedDescription.UTF8String);
        return s;
    }

    void draw(id<MTLTexture> target, id<MTLRenderPipelineState> s, id<MTLTexture> source, const void *bytes,
              size_t length)
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:s];
        [e setFragmentTexture:source atIndex:0];
        [e setFragmentBytes:bytes length:length atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }

    std::vector<float> read(id<MTLTexture> t)
    {
        id<MTLComputePipelineState> p =
            [dev newComputePipelineStateWithFunction:[reader newFunctionWithName:@"read_texels"] error:nil];
        const size_t n = t.width * t.height;
        id<MTLBuffer> out = [dev newBufferWithLength:n * 16 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:p];
        [e setTexture:t atIndex:0];
        [e setBuffer:out offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const float *f = static_cast<const float *>(out.contents);
        return std::vector<float>(f, f + 4 * n);
    }

    id<MTLTexture> texture(MTLPixelFormat f, uint32_t w, uint32_t h, MTLTextureUsage usage)
    {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.usage = usage;
        d.storageMode = MTLStorageModeShared;
        return [dev newTextureWithDescriptor:d];
    }
};

// A surface plane as raw_ycbcr.h's facts of a texture of `format` over it
raw::YcbcrFacts factsOf(IOSurfaceRef s, uint32_t format, uint32_t plane)
{
    raw::YcbcrFacts f;
    f.pixelFormat = format;
    f.fourcc = IOSurfaceGetPixelFormat(s);
    f.plane = plane;
    f.planes = uint32_t(IOSurfaceGetPlaneCount(s));
    f.width = uint32_t(IOSurfaceGetWidthOfPlane(s, plane));
    f.height = uint32_t(IOSurfaceGetHeightOfPlane(s, plane));
    f.surfaceWidth = uint32_t(IOSurfaceGetWidth(s));
    f.surfaceHeight = uint32_t(IOSurfaceGetHeight(s));
    const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
    for (size_t k = 0; k < 2; k++) {
        f.offset[k] = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, k)) - base;
        f.pitch[k] = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, k));
    }
    f.allocBytes = IOSurfaceGetAllocSize(s);
    f.simple = true;
    f.target = true;
    return f;
}

// The plane's words, row by row, `words` a row
std::vector<uint32_t> wordsOf(IOSurfaceRef s, uint32_t plane, uint32_t words)
{
    std::vector<uint32_t> out;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    const uint8_t *b = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
    for (size_t y = 0; y < IOSurfaceGetHeightOfPlane(s, plane); y++) {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(b + y * IOSurfaceGetBytesPerRowOfPlane(s, plane));
        out.insert(out.end(), row, row + words);
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return out;
}

void putWords(IOSurfaceRef s, uint32_t plane, uint32_t words, const uint32_t *from, std::mt19937 *rng)
{
    IOSurfaceLock(s, 0, nullptr);
    uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
    const size_t pitch = IOSurfaceGetBytesPerRowOfPlane(s, plane);
    for (size_t y = 0; y < IOSurfaceGetHeightOfPlane(s, plane); y++)
        for (size_t x = 0; x < pitch / 4; x++) {
            const uint32_t w = from && x < words ? from[y * words + x] : rng ? uint32_t((*rng)()) : 0;
            memcpy(b + y * pitch + 4 * x, &w, 4);
        }
    IOSurfaceUnlock(s, 0, nullptr);
}

std::vector<float> standIn(uint32_t n, std::mt19937 &rng)
{
    std::vector<float> v(4 * n);
    for (float &x : v) {
        const uint32_t k = rng();
        float base;
        if (k % 4 == 0) {
            const float special[] = {NAN, INFINITY, -INFINITY, -0.0f, -0.3f, 1.7f, 0.0f, 1.0f};
            x = special[(k >> 8) % 8];
            continue;
        }
        if (k % 4 == 1) {
            _Float16 a, b;
            const uint16_t h = uint16_t(k >> 8) % 0x3c00, h1 = h + 1;
            memcpy(&a, &h, 2);
            memcpy(&b, &h1, 2);
            base = float((double(a) + double(b)) / 2);
        } else {
            base = float((double((k >> 8) % 1023) + 0.5) / 1023.0);
        }
        uint32_t u;
        memcpy(&u, &base, 4);
        u += uint32_t(int((k >> 3) % 7) - 3);
        memcpy(&x, &u, 4);
    }
    return v;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: packedplaneoracle <raw/pack directory>\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSString *dir = @(argv[1]);
        auto source = [&](NSString *name) {
            NSString *text = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:name]
                                                       encoding:NSUTF8StringEncoding
                                                          error:nil];
            NSString *knots = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"srgb_knots.h"]
                                                        encoding:NSUTF8StringEncoding
                                                           error:nil];
            return [text stringByReplacingOccurrencesOfString:@"#include \"srgb_knots.h\"" withString:knots ?: @""];
        };
        MTLCompileOptions *exact = [MTLCompileOptions new];
        exact.mathMode = MTLMathModeSafe;
        NSError *e = nil;
        g.conversion = [g.dev newLibraryWithSource:source(@"ycbcr.metal") options:nil error:&e];
        g.store = [g.dev newLibraryWithSource:source(@"ycbcr_store.metal") options:exact error:&e];
        g.reader = [g.dev newLibraryWithSource:kReader options:nil error:&e];
        if (!g.conversion || !g.store || !g.reader) {
            fprintf(stderr, "libraries: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLRenderPipelineState> conversion =
            g.state(g.conversion, @"ycbcr_vert", @"ycbcr_packed_plane_frag", MTLPixelFormatRGB10A2Unorm);
        id<MTLRenderPipelineState> store =
            g.state(g.store, @"ycbcr_store_vert", @"ycbcr_store_plane_packed_frag", MTLPixelFormatR32Uint);
        if (!conversion || !store)
            return 1;
        const OSType fourccs[] = {'p420', 'pf20', 'p422', 'pf22', 'p444', 'pf44'};
        const uint32_t widths[] = {96, 97, 98, 130, 31};
        std::mt19937 rng(588);
        uint32_t planes = 0, readBad = 0, convBad = 0, storeBad = 0, roundBad = 0, texels = 0;
        for (const OSType fourcc : fourccs)
            for (const uint32_t width : widths) {
                CVPixelBufferRef pb = nullptr;
                NSDictionary *attrs =
                    @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                      (id)kCVPixelBufferMetalCompatibilityKey : @YES};
                if (CVPixelBufferCreate(nullptr, width, 6, fourcc, (__bridge CFDictionaryRef)attrs, &pb)) {
                    printf("no %u-wide surface of fourcc %08x\n", width, unsigned(fourcc));
                    return 1;
                }
                IOSurfaceRef s = CVPixelBufferGetIOSurface(pb);
                for (uint32_t format : {588u, 589u}) {
                    const uint32_t plane = format - 588;
                    raw::YcbcrPlan plan;
                    const raw::YcbcrFacts facts = factsOf(s, format, plane);
                    if (const char *why = raw::planYcbcr(facts, plan)) {
                        printf("%u over plane %u of %08x (%u wide): refused: %s\n", format, plane, unsigned(fourcc),
                               width, why);
                        readBad++;
                        continue;
                    }
                    const uint32_t w = facts.width, h = facts.height, channels = plan.shape[2], words = plan.lumaWidth;
                    MTLTextureDescriptor *d =
                        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                           width:w
                                                                          height:h
                                                                       mipmapped:NO];
                    d.usage = MTLTextureUsageShaderRead;
                    id<MTLTexture> metal = [g.dev newTextureWithDescriptor:d iosurface:s plane:plane];
                    id<MTLTexture> packed = g.texture(MTLPixelFormatR32Uint, words, h, MTLTextureUsageShaderRead);
                    id<MTLTexture> image = g.texture(MTLPixelFormatRGB10A2Unorm, w, h,
                                                     MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
                    planes++;
                    for (int round = 0; round < 3; round++) {
                        putWords(s, plane, words, nullptr, &rng);
                        const std::vector<uint32_t> held = wordsOf(s, plane, words);
                        [packed replaceRegion:MTLRegionMake2D(0, 0, words, h)
                                  mipmapLevel:0
                                    withBytes:held.data()
                                  bytesPerRow:words * 4];
                        g.draw(image, conversion, packed, plan.shape, sizeof plan.shape);
                        const std::vector<float> want = g.read(metal), got = g.read(image);
                        for (uint32_t i = 0; i < w * h; i++) {
                            uint32_t codes[2];
                            raw::packedPlaneTexel(&held[(i / w) * words], i % w, channels, codes);
                            const float model[4] = {float(codes[0]) / 1023.0f, float(codes[1]) / 1023.0f, 0, 1};
                            readBad += memcmp(model, &want[4 * i], 16) != 0;
                            convBad += memcmp(&got[4 * i], &want[4 * i], 16) != 0;
                            texels++;
                        }
                    }
                    const raw::YcbcrStoreShape shape = plan.store;
                    id<MTLTexture> stand =
                        g.texture(MTLPixelFormat(raw::ycbcrStandInFormat(format)), w, h, MTLTextureUsageShaderRead);
                    id<MTLTexture> stored = g.texture(MTLPixelFormatR32Uint, words, h, MTLTextureUsageRenderTarget);
                    for (int round = 0; round < 3; round++) {
                        const std::vector<float> v = standIn(w * h, rng);
                        std::vector<float> held(channels * w * h);
                        for (uint32_t i = 0; i < w * h; i++)
                            memcpy(&held[channels * i], &v[4 * i], 4 * channels);
                        [stand replaceRegion:MTLRegionMake2D(0, 0, w, h)
                                 mipmapLevel:0
                                   withBytes:held.data()
                                 bytesPerRow:w * 4 * channels];
                        g.draw(stored, store, stand, &shape, sizeof shape);
                        std::vector<uint32_t> got(words * h), want(words * h);
                        [stored getBytes:got.data()
                             bytesPerRow:words * 4
                              fromRegion:MTLRegionMake2D(0, 0, words, h)
                             mipmapLevel:0];
                        raw::packedPlaneDrawn(shape, v.data(), w, h, want.data());
                        storeBad += got != want;
                        putWords(s, plane, words, got.data(), nullptr);
                        const std::vector<float> back = g.read(metal);
                        for (uint32_t i = 0; i < w * h; i++)
                            for (uint32_t c = 0; c < channels; c++)
                                roundBad +=
                                    back[4 * i + c] !=
                                    float(raw::ycbcrTexelCode(v[4 * i + c], raw::YcbcrTexelSource::DrawnFloat, 1023)) /
                                        1023.0f;
                    }
                }
                CVPixelBufferRelease(pb);
            }
        printf("%s: %u planes, %u texels read: %u off packedPlaneTexel, %u of the conversion state off Metal's; store "
               "state: %u passes off packedPlaneDrawn, %u channels read back off the 10-bit rule\n",
               g.dev.name.UTF8String, planes, texels, readBad, convBad, storeBad, roundBad);
        return readBad || convBad || storeBad || roundBad ? 1 : 0;
    }
}
