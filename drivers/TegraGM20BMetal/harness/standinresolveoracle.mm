// SPDX-License-Identifier: MIT
// How an M4 Max resolves a multisampled texture of a format the GM20B renders through a stand-in (format_standin.h),
// against rules over the codes each sample stores (each sample's code calibrated by a one-sample draw of the same value
// into the format) and against the stand-in route: the samples resolved in the stand-in's format, then stored into the
// format (attachment_target.h formatResolveState). RGB9E5Float also against its samples' decoded mean.
// usage: standinresolveoracle <format> <samples>   (formats 11, 41, 42, 44, 93, 553, 555; samples 2 or 4; ARGB8Unorm and
// its sRGB form are harness/resolvecodesoracle.mm's)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr NSUInteger W = 64, H = 64;

NSString *const kSource =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct V { float4 p [[position]]; };\n"
    @"vertex V cover(uint v [[vertex_id]]) { V o; o.p = float4((v & 1) ? 1.0 : -1.0, (v & 2) ? 1.0 : -1.0, 0, 1); "
    @"return o; }\n"
    @"fragment float4 per_sample(V in [[stage_in]], uint s [[sample_id]], constant float4 *v [[buffer(0)]], "
    @"constant uint &n [[buffer(1)]]) {\n"
    @"  uint2 p = uint2(in.p.xy); return v[(p.y * 64 + p.x) * n + s]; }\n"
    @"fragment float4 per_pixel(V in [[stage_in]], constant float4 *v [[buffer(0)]]) {\n"
    @"  uint2 p = uint2(in.p.xy); return v[p.y * 64 + p.x]; }\n";

id<MTLDevice> gDevice;
id<MTLLibrary> gLibrary;
id<MTLCommandQueue> gQueue;

id<MTLRenderPipelineState> state(MTLPixelFormat f, NSUInteger samples, const char *fragment)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [gLibrary newFunctionWithName:@"cover"];
    d.fragmentFunction = [gLibrary newFunctionWithName:@(fragment)];
    d.colorAttachments[0].pixelFormat = f;
    d.rasterSampleCount = samples;
    NSError *e = nil;
    id<MTLRenderPipelineState> s = [gDevice newRenderPipelineStateWithDescriptor:d error:&e];
    if (!s)
        printf("no state of %lu: %s\n", (unsigned long)f, e.localizedDescription.UTF8String);
    return s;
}

id<MTLTexture> texture(MTLPixelFormat f, NSUInteger samples)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:W height:H mipmapped:NO];
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [gDevice newTextureWithDescriptor:d];
}

std::vector<uint8_t> bytesOf(id<MTLTexture> t, NSUInteger bpp)
{
    id<MTLBuffer> b = [gDevice newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(W, H, 1)
                        toBuffer:b
               destinationOffset:0
          destinationBytesPerRow:W * bpp
        destinationBytesPerImage:W * H * bpp];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t *p = static_cast<const uint8_t *>(b.contents);
    return {p, p + W * H * bpp};
}

void draw(id<MTLTexture> into, id<MTLTexture> resolve, id<MTLRenderPipelineState> s, const std::vector<float> &v,
          uint32_t n)
{
    id<MTLBuffer> b = [gDevice newBufferWithBytes:v.data() length:v.size() * 4 options:0];
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = into;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = resolve ? MTLStoreActionMultisampleResolve : MTLStoreActionStore;
    p.colorAttachments[0].resolveTexture = resolve;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:s];
    [e setFragmentBuffer:b offset:0 atIndex:0];
    [e setFragmentBytes:&n length:4 atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
}

struct Fields {
    NSUInteger bpp;
    int count;
    int shift[4], bits[4];
};

Fields fieldsOf(uint32_t f)
{
    switch (f) {
    case 11:
        return {1, 1, {0}, {8}};
    case 41:  // A1BGR5Unorm: red 11, green 6, blue 1, alpha 0
        return {2, 4, {11, 6, 1, 0}, {5, 5, 5, 1}};
    case 42:  // ABGR4Unorm
        return {2, 4, {12, 8, 4, 0}, {4, 4, 4, 4}};
    case 44:  // BGRA4Unorm
        return {2, 4, {8, 4, 0, 12}, {4, 4, 4, 4}};
    case 555:  // red 20, green 10, blue 0
        return {4, 3, {20, 10, 0}, {10, 10, 10}};
    case 553:  // blue, green, red, alpha fields, code in the top 10 bits of each
        return {8, 4, {38, 22, 6, 54}, {10, 10, 10, 10}};
    default:
        return {4, 4, {0, 8, 16, 24}, {8, 8, 8, 8}};
    }
}

uint64_t wordAt(const std::vector<uint8_t> &b, size_t t, NSUInteger bpp)
{
    uint64_t w = 0;
    memcpy(&w, &b[t * bpp], bpp);
    return w;
}

}  // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    @autoreleasepool {
        if (argc < 3)
            return 2;
        const uint32_t format = uint32_t(atoi(argv[1])), samples = uint32_t(atoi(argv[2]));
        gDevice = MTLCreateSystemDefaultDevice();
        gQueue = [gDevice newCommandQueue];
        NSError *error = nil;
        gLibrary = [gDevice newLibraryWithSource:kSource options:nil error:&error];
        const Fields k = fieldsOf(format);
        const bool xr = format == 553 || format == 555;
        id<MTLTexture> ms = texture(MTLPixelFormat(format), samples), into = texture(MTLPixelFormat(format), 1);
        if (!ms || !into) {
            printf("%u x%u: no texture\n", format, samples);
            return 0;
        }
        std::mt19937 rng(format * 131 + samples);
        std::vector<float> values(W * H * samples * 4);
        for (size_t i = 0; i < values.size(); i++) {
            const float u = float(rng() % 65536) / 65535.0f;
            values[i] = xr ? (i % 4 == 3 ? u : u * 1.5f - 0.25f) : format == 93 ? u * u * 100.0f : u;
        }
        id<MTLRenderPipelineState> perSample = state(MTLPixelFormat(format), samples, "per_sample"),
                                   perPixel = state(MTLPixelFormat(format), 1, "per_pixel");
        if (!perSample || !perPixel)
            return 0;
        draw(ms, into, perSample, values, samples);
        const std::vector<uint8_t> got = bytesOf(into, k.bpp);
        std::vector<std::vector<uint8_t>> codes(samples);
        for (uint32_t s = 0; s < samples; s++) {
            std::vector<float> one(W * H * 4);
            for (size_t t = 0; t < W * H; t++)
                for (int c = 0; c < 4; c++)
                    one[t * 4 + c] = values[(t * samples + s) * 4 + c];
            id<MTLTexture> single = texture(MTLPixelFormat(format), 1);
            draw(single, nil, perPixel, one, 1);
            codes[s] = bytesOf(single, k.bpp);
        }
        size_t up[4] = {}, down[4] = {}, even[4] = {}, off1[4] = {}, ties[4] = {}, tiesUp[4] = {};
        int maxOff[4] = {};
        for (size_t t = 0; t < W * H; t++)
            for (int c = 0; c < k.count; c++) {
                const uint64_t mask = (1ull << k.bits[c]) - 1;
                int64_t sum = 0;
                for (uint32_t s = 0; s < samples; s++)
                    sum += int64_t(wordAt(codes[s], t, k.bpp) >> k.shift[c] & mask);
                const int64_t g = int64_t(wordAt(got, t, k.bpp) >> k.shift[c] & mask);
                const int64_t u = (2 * sum + samples) / (2 * samples), dn = (2 * sum + samples - 1) / (2 * samples);
                const int64_t ev = int64_t(std::nearbyint(double(sum) / samples));
                up[c] += g == u;
                down[c] += g == dn;
                even[c] += g == ev;
                const int off = int(std::llabs(g - u));
                off1[c] += off <= 1;
                maxOff[c] = std::max(maxOff[c], off);
                if (u != dn) {
                    ties[c]++;
                    tiesUp[c] += g == u;
                }
            }
        // the stand-in route: the same samples drawn into the stand-in's format and resolved there, the resolve drawn into
        // a one-sample texture of the format (its store), compared byte for byte
        const uint32_t standIn = format == 11 ? 71 : (format == 553 || format == 555) ? 115 : 125;
        id<MTLTexture> sms = texture(MTLPixelFormat(standIn), samples), sinto = texture(MTLPixelFormat(standIn), 1);
        id<MTLRenderPipelineState> sPerSample = state(MTLPixelFormat(standIn), samples, "per_sample");
        draw(sms, sinto, sPerSample, values, samples);
        const NSUInteger sbpp = standIn == 71 ? 4 : standIn == 115 ? 8 : 16;
        const std::vector<uint8_t> resolved = bytesOf(sinto, sbpp);
        size_t sameStandIn = 0;
        if (standIn == 71) {
            for (size_t t = 0; t < W * H; t++)
                sameStandIn += got[t] == resolved[t * 4];
        } else {
            std::vector<float> mean(W * H * 4);
            for (size_t t = 0; t < W * H; t++)
                for (int c = 0; c < 4; c++) {
                    if (standIn == 115) {
                        _Float16 h;
                        memcpy(&h, &resolved[t * 8 + 2 * c], 2);
                        mean[t * 4 + c] = float(h);
                    } else {
                        memcpy(&mean[t * 4 + c], &resolved[t * 16 + 4 * c], 4);
                    }
                }
            id<MTLTexture> stored = texture(MTLPixelFormat(format), 1);
            draw(stored, nil, perPixel, mean, 1);
            const std::vector<uint8_t> back = bytesOf(stored, k.bpp);
            size_t near = 0;
            for (size_t t = 0; t < W * H; t++) {
                sameStandIn += !memcmp(&got[t * k.bpp], &back[t * k.bpp], k.bpp);
                bool within = true;
                for (int c = 0; c < k.count; c++) {
                    const uint64_t mask = (1ull << k.bits[c]) - 1;
                    within &= std::llabs(int64_t(wordAt(got, t, k.bpp) >> k.shift[c] & mask) -
                                         int64_t(wordAt(back, t, k.bpp) >> k.shift[c] & mask)) <= 1;
                }
                near += within;
            }
            printf("%u x%u stand-in route (resolve in %u, then stored): texels within one code %zu\n", format, samples,
                   standIn, near);
            if (format == 93) {
                double worst = 0;
                size_t one = 0;
                for (size_t t = 0; t < W * H; t++) {
                    const uint32_t a = uint32_t(wordAt(got, t, 4)), b = uint32_t(wordAt(back, t, 4));
                    const double ulp = std::ldexp(1.0, int(a >> 27) - 24);
                    double off = 0;
                    for (int c = 0; c < 3; c++)
                        off = std::max(off, std::fabs(std::ldexp(double(a >> (9 * c) & 511), int(a >> 27) - 24) -
                                                      std::ldexp(double(b >> (9 * c) & 511), int(b >> 27) - 24)) /
                                                ulp);
                    worst = std::max(worst, off);
                    one += off <= 1.0;
                }
                printf("93 x%u stand-in route: texels within one mantissa code of Apple's %zu, max %.2f codes\n",
                       samples, one, worst);
                const auto encode = [](const double in[3], bool rounded) {
                    double c[3];
                    for (int k = 0; k < 3; k++)
                        c[k] = in[k] > 0 ? std::fmin(in[k], 65408.0) : 0.0;
                    const double m = std::fmax(c[0], std::fmax(c[1], c[2]));
                    const float mf = float(m);
                    uint32_t bits;
                    memcpy(&bits, &mf, 4);
                    int e = m > 0 ? std::max(0, int(bits >> 23 & 255) - 127 + 16) : 0;
                    double scale = std::ldexp(1.0, e - 24);
                    if (rounded && std::floor(m / scale + 0.5) >= 512.0) {
                        e++;
                        scale *= 2;
                    }
                    uint32_t w = uint32_t(e) << 27;
                    for (int k = 0; k < 3; k++)
                        w |= uint32_t(rounded ? std::floor(c[k] / scale + 0.5) : std::floor(c[k] / scale)) << (9 * k);
                    return w;
                };
                size_t truncHits = 0, roundHits = 0, truncOne = 0, roundOne = 0;
                for (size_t t = 0; t < W * H; t++) {
                    double m[3] = {0, 0, 0};
                    for (uint32_t s2 = 0; s2 < samples; s2++) {
                        const uint32_t w = uint32_t(wordAt(codes[s2], t, 4));
                        for (int c = 0; c < 3; c++)
                            m[c] += std::ldexp(double(w >> (9 * c) & 511), int(w >> 27) - 24) / samples;
                    }
                    const uint32_t a = uint32_t(wordAt(got, t, 4));
                    for (int r = 0; r < 2; r++) {
                        const uint32_t b = encode(m, r == 1);
                        const double ulp = std::ldexp(1.0, int(a >> 27) - 24);
                        double off = 0;
                        for (int c = 0; c < 3; c++)
                            off = std::max(off, std::fabs(std::ldexp(double(a >> (9 * c) & 511), int(a >> 27) - 24) -
                                                          std::ldexp(double(b >> (9 * c) & 511), int(b >> 27) - 24)) /
                                                    ulp);
                        (r ? roundHits : truncHits) += a == b;
                        (r ? roundOne : truncOne) += off <= 1.0;
                    }
                }
                printf("93 x%u decoded samples' mean: truncated exact %zu within one %zu; rounded exact %zu within one "
                       "%zu\n",
                       samples, truncHits, truncOne, roundHits, roundOne);
            }
        }
        printf("%u x%u stand-in route (resolve in %u, then stored): exact texels %zu of %zu\n", format, samples,
               standIn, sameStandIn, size_t(W * H));
        for (int c = 0; c < k.count; c++)
            printf("%u x%u field %d (%d bits): code mean half up %zu, ties down %zu, half even %zu of %zu; within one "
                   "%zu, max off %d; ties %zu, of them up %zu\n",
                   format, samples, c, k.bits[c], up[c], down[c], even[c], size_t(W * H), off1[c], maxOff[c], ties[c],
                   tiesUp[c]);
    }
    return 0;
}
