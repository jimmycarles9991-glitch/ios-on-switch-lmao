// SPDX-License-Identifier: MIT
// raw GM20B path: pack/ycbcr.metal's conversion and raw_ycbcr.cpp's plan of it, run on this Mac's own GPU, against what this Mac's
// Metal reads of the same two-plane YCbCr textures (8- and 10-bit and packed, video and full range, 4:2:0, 4:2:2 and 4:4:4
// surfaces and formats, converted, sRGB and codes, no matrix, BT.709 and BT.2020, over plane 0 and plane 1), over CoreVideo
// surfaces of random codes: every texel's channels must be Metal's. The raw path runs the same conversion on the GM20B.
// usage: ycbcroracle <pack/ycbcr.metal>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <random>
#include <vector>

#include "../runtime/raw_ycbcr.h"

namespace {
NSString *const kReader =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"kernel void read_texels(texture2d<float, access::read> t [[texture(0)]], device float4 *out "
    @"[[buffer(0)]], uint2 at [[thread_position_in_grid]]) {\n"
    @"  if (at.x < t.get_width() && at.y < t.get_height()) out[at.y * t.get_width() + at.x] = t.read(at);\n}\n";

id<MTLLibrary> conversionLibrary(id<MTLDevice> dev, const char *path)
{
    NSString *metal = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    NSString *dir = [@(path) stringByDeletingLastPathComponent];
    NSString *knots = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"srgb_knots.h"]
                                                encoding:NSUTF8StringEncoding
                                                   error:nil];
    if (!metal || !knots)
        return nil;
    metal = [metal stringByReplacingOccurrencesOfString:@"#include \"srgb_knots.h\"" withString:knots];
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:metal options:nil error:&e];
    if (!lib)
        fprintf(stderr, "ycbcr.metal: %s\n", e.localizedDescription.UTF8String);
    return lib;
}

struct Case {
    OSType fourcc;
    uint32_t format, plane, w, h;
    const char *matrix;
};

const char *outputName(raw::YcbcrOutput o)
{
    static const char *const names[] = {"",        "converted",        "srgb",        "codes8",
                                        "codes10", "converted packed", "srgb packed", "codes10 packed"};
    return names[o];
}

// Metal's texels of the YCbCr texture, each channel as a whole step (v of v / 2048, step of step / 8192, or the code)
std::vector<uint32_t> metalTexels(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLComputePipelineState> reader,
                                  id<MTLTexture> t, const raw::YcbcrPlan &p)
{
    const NSUInteger n = t.width * t.height;
    id<MTLBuffer> out = [dev newBufferWithLength:n * 16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:reader];
    [en setTexture:t atIndex:0];
    [en setBuffer:out offset:0 atIndex:0];
    [en dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [en endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const bool converted = p.output == raw::YcbcrConverted || p.output == raw::YcbcrConvertedPacked;
    const bool srgb = p.output == raw::YcbcrSrgb || p.output == raw::YcbcrSrgbPacked;
    const float scale = converted ? 2048.0f : srgb ? 8192.0f : float(p.shape[12]);
    std::vector<uint32_t> v;
    const float *f = static_cast<const float *>(out.contents);
    for (NSUInteger i = 0; i < n; i++)
        for (int k = 0; k < 4; k++)
            v.push_back(uint32_t(lrintf(f[4 * i + k] * (k == 3 ? 1.0f : scale))));
    return v;
}

// pack/ycbcr.metal's texels from plane textures the plan names, read back alike
std::vector<uint32_t> ourTexels(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib, IOSurfaceRef s,
                                const raw::YcbcrPlan &p, uint32_t w, uint32_t h)
{
    const uint8_t *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(s));
    auto plane = [&](uint32_t format, uint64_t at, uint32_t pitch, uint32_t pw, uint32_t ph) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                     width:pw
                                                                                    height:ph
                                                                                 mipmapped:NO];
        id<MTLTexture> t = [dev newTextureWithDescriptor:d];
        [t replaceRegion:MTLRegionMake2D(0, 0, pw, ph) mipmapLevel:0 withBytes:base + at bytesPerRow:pitch];
        return t;
    };
    id<MTLTexture> luma = plane(p.lumaFormat, p.luma, p.lumaPitch, p.lumaWidth, h);
    id<MTLTexture> chroma =
        p.chromaWidth ? plane(p.chromaFormat, p.chroma, p.chromaPitch, p.chromaWidth, p.chromaHeight) : luma;
    static const char *const frags[] = {"",
                                        "ycbcr_converted_frag",
                                        "ycbcr_srgb_frag",
                                        "ycbcr_codes_frag",
                                        "ycbcr_codes10_frag",
                                        "ycbcr_packed_converted_frag",
                                        "ycbcr_packed_srgb_frag",
                                        "ycbcr_packed_codes10_frag"};
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"ycbcr_vert"];
    pd.fragmentFunction = [lib newFunctionWithName:@(frags[p.output])];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormat(p.image);
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) {
        fprintf(stderr, "pipeline: %s\n", e.localizedDescription.UTF8String);
        return {};
    }
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(p.image)
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    od.usage = MTLTextureUsageRenderTarget;
    od.storageMode = MTLStorageModeShared;
    id<MTLTexture> image = [dev newTextureWithDescriptor:od];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = image;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps];
    [en setFragmentTexture:luma atIndex:0];
    [en setFragmentTexture:chroma atIndex:1];
    [en setFragmentBytes:p.shape length:sizeof p.shape atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [en endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const size_t bpp = p.image == 115 ? 8 : p.image == 125 ? 16 : 4;
    std::vector<uint8_t> px(size_t(w) * h * bpp);
    [image getBytes:px.data() bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    std::vector<uint32_t> v;
    for (size_t i = 0; i < size_t(w) * h; i++) {
        const uint8_t *t = &px[i * bpp];
        for (int k = 0; k < 4; k++) {
            if (p.image == 115) {
                __fp16 f;
                memcpy(&f, t + 2 * k, 2);
                v.push_back(uint32_t(lrintf(float(f) * (k == 3 ? 1.0f : 2048.0f))));
            } else if (p.image == 125) {
                float f;
                memcpy(&f, t + 4 * k, 4);
                v.push_back(uint32_t(lrintf(f * (k == 3 ? 1.0f : 8192.0f))));
            } else if (p.image == 70) {
                v.push_back(k == 3 ? t[k] / 255u : t[k]);
            } else {
                uint32_t word;
                memcpy(&word, t, 4);
                v.push_back(k == 3 ? (word >> 30) / 3u : (word >> (10 * k)) & 1023);
            }
        }
    }
    return v;
}

// A surface of random codes (a 10-bit plane's words random in their low bits too: Metal reads the high 10)
CVPixelBufferRef surfaceOf(const Case &c, std::mt19937 &rng)
{
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferCreate(nullptr, c.w, c.h, c.fourcc, (__bridge CFDictionaryRef)attrs, &pb))
        return nullptr;
    IOSurfaceRef s = CVPixelBufferGetIOSurface(pb);
    IOSurfaceLock(s, 0, nullptr);
    uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(s));
    for (size_t i = 0; i < IOSurfaceGetAllocSize(s); i++)
        b[i] = uint8_t(rng());
    IOSurfaceUnlock(s, 0, nullptr);
    if (c.matrix)
        IOSurfaceSetValue(s, CFSTR("IOSurfaceYCbCrMatrix"),
                          (__bridge CFStringRef)[NSString stringWithUTF8String:c.matrix]);
    return pb;
}

raw::YcbcrFacts factsOf(const Case &c, IOSurfaceRef s)
{
    raw::YcbcrFacts f;
    f.pixelFormat = c.format;
    f.fourcc = IOSurfaceGetPixelFormat(s);
    f.plane = c.plane;
    f.planes = uint32_t(IOSurfaceGetPlaneCount(s));
    f.width = c.w;
    f.height = c.h;
    f.surfaceWidth = uint32_t(IOSurfaceGetWidth(s));
    f.surfaceHeight = uint32_t(IOSurfaceGetHeight(s));
    const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
    for (size_t k = 0; k < 2; k++) {
        f.offset[k] = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, k)) - base;
        f.pitch[k] = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, k));
    }
    f.allocBytes = IOSurfaceGetAllocSize(s);
    f.simple = true;
    f.matrix = c.matrix;
    return f;
}

std::vector<Case> cases()
{
    struct Surface {
        OSType fourcc;
        int bits;  // 8, 10, or 30: 10-bit codes packed three to a word
        int sub;   // 0 4:2:0, 1 4:2:2, 2 4:4:4
    };

    const Surface surfaces[] = {{'420v', 8, 0},  {'420f', 8, 0},  {'422v', 8, 1},  {'422f', 8, 1},  {'444v', 8, 2},
                                {'444f', 8, 2},  {'x420', 10, 0}, {'xf20', 10, 0}, {'x422', 10, 1}, {'xf22', 10, 1},
                                {'x444', 10, 2}, {'xf44', 10, 2}, {'p420', 30, 0}, {'pf20', 30, 0}, {'p422', 30, 1},
                                {'pf22', 30, 1}, {'p444', 30, 2}, {'pf44', 30, 2}};
    const char *const matrices[] = {nullptr, "ITU_R_709_2", "ITU_R_2020"};
    const uint32_t sizes[][2] = {{33, 17}, {64, 48}, {7, 5}};
    std::vector<Case> out;
    int n = 0;
    for (const Surface &s : surfaces)
        for (int sub = 0; sub <= s.sub; sub++)  // the format's chroma no finer than the surface's
            for (int reads = 0; reads < 3; reads++) {
                static const uint32_t formats[3][3][3] = {{{500, 502, 503}, {505, 506, 507}, {508, 509, 510}},
                                                          {{520, 522, 523}, {525, 526, 527}, {528, 529, 530}},
                                                          {{540, 541, 542}, {543, 544, 545}, {546, 547, 548}}};
                const uint32_t format = formats[reads][s.bits == 8 ? 0 : s.bits == 10 ? 1 : 2][sub];
                for (const char *m : matrices) {
                    const uint32_t *size = sizes[n++ % 3];
                    out.push_back({s.fourcc, format, 0, size[0], size[1], m});
                    if (s.sub && sub == s.sub)  // plane 1 of a surface whose chroma rows are its luma rows
                        out.push_back({s.fourcc, format, 1, size[0], size[1], m});
                }
            }
    return out;
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: ycbcroracle <pack/ycbcr.metal>\n");
            return 2;
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLLibrary> lib = conversionLibrary(dev, argv[1]);
        NSError *e = nil;
        id<MTLLibrary> readLib = [dev newLibraryWithSource:kReader options:nil error:&e];
        id<MTLComputePipelineState> reader =
            [dev newComputePipelineStateWithFunction:[readLib newFunctionWithName:@"read_texels"] error:&e];
        if (!lib || !reader)
            return 1;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        std::mt19937 rng(20261106);
        int failed = 0, run = 0;
        for (const Case &c : cases()) {
            CVPixelBufferRef pb = surfaceOf(c, rng);
            if (!pb) {
                printf("FAIL no %.4s surface\n", (const char *)&c.fourcc);
                failed++;
                continue;
            }
            IOSurfaceRef s = CVPixelBufferGetIOSurface(pb);
            const char fc[5] = {char(c.fourcc >> 24), char(c.fourcc >> 16), char(c.fourcc >> 8), char(c.fourcc), 0};
            raw::YcbcrPlan p;
            const raw::YcbcrFacts facts = factsOf(c, s);
            if (const char *why = raw::planYcbcr(facts, p)) {
                printf("FAIL %u over '%s' plane %u %ux%u: not planned (%s)\n", c.format, fc, c.plane, c.w, c.h, why);
                failed++;
                CVPixelBufferRelease(pb);
                continue;
            }
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(c.format)
                                                                                         width:c.w
                                                                                        height:c.h
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:c.plane];
            if (!t) {
                printf("FAIL %u over '%s' plane %u: Metal makes no texture\n", c.format, fc, c.plane);
                failed++;
                CVPixelBufferRelease(pb);
                continue;
            }
            const std::vector<uint32_t> metal = metalTexels(dev, q, reader, t, p);
            const std::vector<uint32_t> ours = ourTexels(dev, q, lib, s, p, c.w, c.h);
            size_t off = 0, first = metal.size();
            for (size_t i = 0; i < metal.size() && i < ours.size(); i++)
                if (metal[i] != ours[i]) {
                    off++;
                    first = std::min(first, i);
                }
            const bool ok = !off && ours.size() == metal.size();
            run++;
            failed += !ok;
            printf("%s %u (%s) over '%s' plane %u %ux%u %s: %zu of %zu channels off", ok ? "OK  " : "FAIL", c.format,
                   outputName(p.output), fc, c.plane, c.w, c.h, c.matrix ?: "no matrix", off, metal.size());
            if (off)
                printf(" (first: texel %zu channel %zu, Metal %u, ours %u)", first / 4, first % 4, metal[first],
                       ours[first]);
            printf("\n");
            CVPixelBufferRelease(pb);
        }
        printf("ycbcroracle: %d of %d differ from Metal's reads\n", failed, run);
        return failed ? 1 : 0;
    }
}
