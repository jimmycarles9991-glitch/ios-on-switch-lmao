// SPDX-License-Identifier: MIT
// raw GM20B path: pack/resolve.metal's resolves, run on this Mac's own GPU, against this Mac's Metal resolving the same multisampled
// textures (MTLStoreActionMultisampleResolve) of random samples, every format the pack resolves at 2 and 4 samples. A format and
// count whose rule is closed must be Metal's in every byte; four halves (and RG11B10Float's channels, rounded from them) follow
// the nearest form measured, whose differing texels are counted only. An 8-bit sRGB resolve writes its codes into the format's
// UNORM twin, as the raw path renders it. usage: resolveoracle <pack/resolve.metal>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <vector>

namespace {
// Per-sample texels from a hash of the pixel and the sample: 8-bit channels any value; half channels any bit pattern (NaNs,
// infinities and denormals among them); RG11B10Float channels a positive float from 2^-20 to 2^16
NSString *const kSamples =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct V { float4 p [[position]]; };\n"
    @"vertex V cover(uint v [[vertex_id]]) { V o; o.p = float4((v & 1) ? 1.0 : -1.0, (v & 2) ? 1.0 : -1.0, 0, 1); "
    @"return o; }\n"
    @"static uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return "
    @"x; }\n"
    @"static uint seed(float4 p, uint s, uint c) { return hash(uint(p.x) * 7919u + uint(p.y) * 104729u + s * 31u + c); "
    @"}\n"
    @"fragment float4 unorm_samples(V in [[stage_in]], uint s [[sample_id]]) {\n"
    @"  return float4(seed(in.p, s, 0) & 255, seed(in.p, s, 1) & 255, seed(in.p, s, 2) & 255, seed(in.p, s, 3) & 255) "
    @"/ 255.0; }\n"
    @"fragment float4 half_samples(V in [[stage_in]], uint s [[sample_id]]) {\n"
    @"  float4 o; for (uint c = 0; c < 4; c++) o[c] = float(as_type<half>(ushort(seed(in.p, s, c)))); return o; }\n"
    @"fragment float4 small_float_samples(V in [[stage_in]], uint s [[sample_id]]) {\n"
    @"  float4 o; for (uint c = 0; c < 4; c++) o[c] = as_type<float>(0x35800000u + seed(in.p, s, c) % 0x12000000u);\n"
    @"  return o; }\n";

struct Case {
    MTLPixelFormat format, written;  // the resolve texture's format, and the one the resolve state writes
    const char *function, *samples;
    bool closed2, closed4;  // the rule at 2 and at 4 samples is closed
};

const Case kCases[] = {
    {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm, "resolve_rgba8_frag", "unorm_samples", true, true},
    {MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm, "resolve_rgba8_srgb_frag", "unorm_samples", true, true},
    {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm, "resolve_bgra8_frag", "unorm_samples", true, true},
    {MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatBGRA8Unorm, "resolve_bgra8_srgb_frag", "unorm_samples", true, true},
    {MTLPixelFormatRG11B10Float, MTLPixelFormatRG11B10Float, "resolve_rg11b10f_frag", "small_float_samples", true,
     false},
    {MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float, "resolve_rgba16f_frag", "half_samples", true, false},
};

constexpr NSUInteger kWidth = 256, kHeight = 256;

id<MTLLibrary> resolveLibrary(id<MTLDevice> dev, const char *path)
{
    NSString *metal = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    NSString *dir = [@(path) stringByDeletingLastPathComponent];
    NSString *tables = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"resolve_srgb.h"]
                                                 encoding:NSUTF8StringEncoding
                                                    error:nil];
    if (!metal || !tables)
        return nil;
    metal = [metal stringByReplacingOccurrencesOfString:@"#include \"resolve_srgb.h\"" withString:tables];
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.mathMode = MTLMathModeSafe;  // rawpack.py builds resolve.metal without fast math
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:metal options:o error:&e];
    if (!lib)
        fprintf(stderr, "resolve.metal: %s\n", e.localizedDescription.UTF8String);
    return lib;
}

id<MTLTexture> texture(id<MTLDevice> dev, MTLPixelFormat f, NSUInteger samples, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                 width:kWidth
                                                                                height:kHeight
                                                                             mipmapped:NO];
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    d.usage = usage;
    d.storageMode = samples > 1 ? MTLStorageModePrivate : MTLStorageModeShared;
    return [dev newTextureWithDescriptor:d];
}

id<MTLRenderPipelineState> state(id<MTLDevice> dev, id<MTLLibrary> lib, const char *vertex, const char *fragment,
                                 MTLPixelFormat f, NSUInteger samples)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@(vertex)];
    d.fragmentFunction = [lib newFunctionWithName:@(fragment)];
    d.colorAttachments[0].pixelFormat = f;
    d.rasterSampleCount = samples;
    NSError *e = nil;
    id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!s)
        fprintf(stderr, "%s: %s\n", fragment, e.localizedDescription.UTF8String);
    return s;
}

std::vector<uint8_t> bytesOf(id<MTLTexture> t)
{
    const NSUInteger row = kWidth * (t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : 4);
    std::vector<uint8_t> out(row * kHeight);
    [t getBytes:out.data() bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, kWidth, kHeight) mipmapLevel:0];
    return out;
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLLibrary> resolves = argc > 1 ? resolveLibrary(dev, argv[1]) : nil;
        NSError *e = nil;
        id<MTLLibrary> samples = [dev newLibraryWithSource:kSamples options:nil error:&e];
        if (!dev || !resolves || !samples)
            return 2;
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        unsigned failed = 0;
        for (const Case &c : kCases)
            for (NSUInteger n : {2u, 4u}) {
                id<MTLTexture> ms = texture(dev, c.format, n, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
                id<MTLTexture> metal = texture(dev, c.format, 1, MTLTextureUsageRenderTarget);
                id<MTLTexture> ours = texture(dev, c.written, 1, MTLTextureUsageRenderTarget);
                id<MTLRenderPipelineState> fill = state(dev, samples, "cover", c.samples, c.format, n);
                id<MTLRenderPipelineState> resolve = state(dev, resolves, "resolve_vert", c.function, c.written, 1);
                if (!ms || !metal || !ours || !fill || !resolve)
                    return 2;
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = ms;
                p.colorAttachments[0].resolveTexture = metal;
                p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                p.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:p];
                [enc setRenderPipelineState:fill];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [enc endEncoding];
                MTLRenderPassDescriptor *r = [MTLRenderPassDescriptor renderPassDescriptor];
                r.colorAttachments[0].texture = ours;
                r.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                r.colorAttachments[0].storeAction = MTLStoreActionStore;
                enc = [cb renderCommandEncoderWithDescriptor:r];
                const uint32_t count = uint32_t(n);
                [enc setRenderPipelineState:resolve];
                [enc setFragmentTexture:ms atIndex:0];
                [enc setFragmentBytes:&count length:sizeof count atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                const std::vector<uint8_t> want = bytesOf(metal), got = bytesOf(ours);
                const size_t texel = want.size() / (kWidth * kHeight);
                size_t differ = 0;
                for (size_t i = 0; i < want.size(); i += texel)
                    differ += memcmp(&want[i], &got[i], texel) != 0;
                const bool rule = n == 2 ? c.closed2 : c.closed4;
                printf("%-24s x%lu: %zu of %lu texels differ from Metal's resolve%s\n", c.function, (unsigned long)n,
                       differ, (unsigned long)(kWidth * kHeight), rule ? "" : " (not closed: counted only)");
                failed += rule && differ;
            }
        printf("resolveoracle: %u case(s) failed\n", failed);
        return failed ? 1 : 0;
    }
}
