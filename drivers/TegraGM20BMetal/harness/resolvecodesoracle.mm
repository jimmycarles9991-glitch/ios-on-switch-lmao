// SPDX-License-Identifier: MIT
// How this Mac's Apple GPU resolves formats the driver has no resolve state for: signed normalized, extended range and the
// formats a stand-in renders. Each sample of a 64x64 multisampled target is drawn its own value in the pass that resolves
// it (MTLStoreActionMultisampleResolve); the resolved texture is compared byte for byte with a one-sample texture drawn the
// samples' mean (`mean`: in float, of the values as drawn), and for the code formats with rules over the codes themselves.
// usage: resolvecodesoracle <format> <samples> [view format the samples are drawn through]
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
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
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

struct Kind {
    int snormBits = 0;  // signed normalized: the channels' bits
    int channels = 4;
    bool xr = false;  // an extended-range code format (code (c - 384) / 510)
    bool halves = false;
    float scale = 1;  // a plain float format's sample range
    NSUInteger bpp = 4;
};

Kind kindOf(uint32_t f)
{
    switch (f) {
    case 12:
        return {8, 1, false, false, 1, 1};
    case 32:
        return {8, 2, false, false, 1, 2};
    case 72:
        return {8, 4, false, false, 1, 4};
    case 22:
        return {16, 1, false, false, 1, 2};
    case 62:
        return {16, 2, false, false, 1, 4};
    case 112:
        return {16, 4, false, false, 1, 8};
    case 552:
    case 553:
        return {0, 4, true, false, 1, 8};
    case 554:
    case 555:
        return {0, 3, true, false, 1, 4};
    case 93:
        return {0, 3, false, true, 100, 4};
    case 11:
        return {0, 1, false, true, 1, 1};
    case 41:
    case 42:
    case 43:
    case 40:
        return {0, 4, false, true, 1, 2};
    default:
        return {0, 4, false, true, 1, 4};
    }
}

}  // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    @autoreleasepool {
        if (argc < 3)
            return 2;
        const uint32_t format = uint32_t(atoi(argv[1])), samples = uint32_t(atoi(argv[2]));
        const uint32_t view = argc > 3 ? uint32_t(atoi(argv[3])) : format;
        gDevice = MTLCreateSystemDefaultDevice();
        gQueue = [gDevice newCommandQueue];
        NSError *error = nil;
        gLibrary = [gDevice newLibraryWithSource:kSource options:nil error:&error];
        const Kind k = kindOf(format);
        id<MTLTexture> ms = texture(MTLPixelFormat(format), samples), into = texture(MTLPixelFormat(format), 1),
                       calibrated = texture(MTLPixelFormat(format), 1);
        if (!ms || !into || !calibrated) {
            printf("%u x%u: no texture\n", format, samples);
            return 0;
        }
        id<MTLTexture> drawn = view == format ? ms : [ms newTextureViewWithPixelFormat:MTLPixelFormat(view)];
        std::mt19937 rng(format * 131 + samples);
        std::vector<float> values(W * H * samples * 4), mean(W * H * 4);
        std::vector<int32_t> codes(W * H * samples * 4);
        for (size_t i = 0; i < values.size(); i++) {
            float v;
            if (k.snormBits) {
                const int32_t max = (1 << (k.snormBits - 1)) - 1;
                const int32_t c = int32_t(rng() % uint32_t(2 * max + 1)) - max;
                codes[i] = c;
                v = float(c) / float(max);
            } else if (k.xr) {
                const int32_t c = (i % 4 == 3 && k.channels == 4) ? 384 + int32_t(rng() % 511) : int32_t(rng() % 1024);
                codes[i] = c;
                v = (float(c) - 384.0f) / 510.0f;
            } else {
                v = float(rng() % 65536) / 65535.0f * k.scale;
                v = float(_Float16(v));
            }
            values[i] = v;
        }
        for (size_t p = 0; p < W * H; p++)
            for (int c = 0; c < 4; c++) {
                float sum = 0;
                for (uint32_t s = 0; s < samples; s++)
                    sum += values[(p * samples + s) * 4 + c];
                mean[p * 4 + c] = sum / float(samples);
            }
        id<MTLRenderPipelineState> perSample = state(MTLPixelFormat(view), samples, "per_sample"),
                                   perPixel = state(MTLPixelFormat(format), 1, "per_pixel");
        if (!perSample || !perPixel)
            return 0;
        id<MTLBuffer> v = [gDevice newBufferWithBytes:values.data() length:values.size() * 4 options:0],
                      m = [gDevice newBufferWithBytes:mean.data() length:mean.size() * 4 options:0];
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = drawn;
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        p.colorAttachments[0].resolveTexture =
            view == format ? into : [into newTextureViewWithPixelFormat:MTLPixelFormat(view)];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        [e setRenderPipelineState:perSample];
        [e setFragmentBuffer:v offset:0 atIndex:0];
        [e setFragmentBytes:&samples length:4 atIndex:1];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        MTLRenderPassDescriptor *q = [MTLRenderPassDescriptor renderPassDescriptor];
        q.colorAttachments[0].texture = calibrated;
        q.colorAttachments[0].loadAction = MTLLoadActionClear;
        q.colorAttachments[0].storeAction = MTLStoreActionStore;
        e = [cb renderCommandEncoderWithDescriptor:q];
        [e setRenderPipelineState:perPixel];
        [e setFragmentBuffer:m offset:0 atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) {
            printf("%u x%u: status %ld\n", format, samples, (long)cb.status);
            return 0;
        }
        const std::vector<uint8_t> got = bytesOf(into, k.bpp), drawnMean = bytesOf(calibrated, k.bpp);
        size_t same = 0;
        for (size_t i = 0; i < W * H; i++)
            same += !memcmp(&got[i * k.bpp], &drawnMean[i * k.bpp], k.bpp);
        if (k.xr)
            for (int t = 0; t < 2; t++) {
                printf("  resolved texel %d bytes:", t);
                for (NSUInteger b = 0; b < k.bpp; b++)
                    printf(" %02x", got[t * k.bpp + b]);
                printf("\n");
            }
        printf("%u x%u%s: resolve == drawn mean in %zu of %zu texels\n", format, samples,
               view != format ? " (drawn through its view)" : "", same, size_t(W * H));
        if (format == 84 || format == 85 || format == 93) {
            std::vector<std::vector<uint8_t>> sampleCodes(samples);
            for (uint32_t si = 0; si < samples; si++) {
                std::vector<float> one(W * H * 4);
                for (size_t t = 0; t < W * H; t++)
                    for (int c = 0; c < 4; c++)
                        one[t * 4 + c] = values[(t * samples + si) * 4 + c];
                id<MTLBuffer> ob = [gDevice newBufferWithBytes:one.data() length:one.size() * 4 options:0];
                id<MTLTexture> single = texture(MTLPixelFormat(format), 1);
                id<MTLCommandBuffer> c1 = [gQueue commandBuffer];
                MTLRenderPassDescriptor *r = [MTLRenderPassDescriptor renderPassDescriptor];
                r.colorAttachments[0].texture = single;
                r.colorAttachments[0].loadAction = MTLLoadActionClear;
                r.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e1 = [c1 renderCommandEncoderWithDescriptor:r];
                [e1 setRenderPipelineState:perPixel];
                [e1 setFragmentBuffer:ob offset:0 atIndex:0];
                [e1 drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [e1 endEncoding];
                [c1 commit];
                [c1 waitUntilCompleted];
                sampleCodes[si] = bytesOf(single, k.bpp);
            }
            if (format != 93) {
                auto dec = [](int c) {
                    const double x = c / 255.0;
                    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
                };
                auto enc = [](double y) {
                    const double x = y <= 0.0031308 ? y * 12.92 : 1.055 * std::pow(y, 1 / 2.4) - 0.055;
                    return int(std::floor(x * 255 + 0.5));
                };
                int maxCode = 0, maxLinear = 0;
                size_t exactCode = 0, exactLinear = 0, n = 0;
                for (size_t t = 0; t < W * H; t++)
                    for (int b = 0; b < 4; b++) {
                        int sum = 0;
                        double lin = 0;
                        for (uint32_t si = 0; si < samples; si++) {
                            sum += sampleCodes[si][t * 4 + b];
                            lin += dec(sampleCodes[si][t * 4 + b]);
                        }
                        const int g = got[t * 4 + b], codeMean = (sum * 2 + int(samples)) / (2 * int(samples));
                        const bool alpha = b == 0;  // ARGB8: its alpha byte first
                        const int linearMean = alpha ? codeMean : enc(lin / samples);
                        maxCode = std::max(maxCode, std::abs(g - codeMean));
                        maxLinear = std::max(maxLinear, std::abs(g - linearMean));
                        exactCode += g == codeMean;
                        exactLinear += g == linearMean;
                        n++;
                    }
                printf("best forms over %zu channels: code mean half up %zu exact, max |off| %d; linear mean (colour) "
                       "%zu exact, max |off| %d\n",
                       n, exactCode, maxCode, exactLinear, maxLinear);
            } else {
                auto decode = [](uint32_t w, float out[3]) {
                    const int e = int(w >> 27) - 15 - 9;
                    for (int c = 0; c < 3; c++)
                        out[c] = std::ldexp(float((w >> (9 * c)) & 511), e);
                };
                double maxUlps = 0;
                size_t exact = 0;
                for (size_t t = 0; t < W * H; t++) {
                    uint32_t gw;
                    memcpy(&gw, &got[t * 4], 4);
                    float gv[3], acc[3] = {0, 0, 0};
                    decode(gw, gv);
                    for (uint32_t si = 0; si < samples; si++) {
                        uint32_t sw;
                        memcpy(&sw, &sampleCodes[si][t * 4], 4);
                        float sv[3];
                        decode(sw, sv);
                        for (int c = 0; c < 3; c++)
                            acc[c] += sv[c];
                    }
                    const float ulp = std::ldexp(1.0f, int(gw >> 27) - 15 - 9);
                    bool same3 = true;
                    for (int c = 0; c < 3; c++) {
                        const double off = std::fabs(double(gv[c]) - double(acc[c]) / samples) / ulp;
                        maxUlps = std::max(maxUlps, off);
                        same3 &= off < 1;
                    }
                    exact += same3;
                }
                printf(
                    "best form (decoded samples' mean): %zu of %zu texels within one mantissa code, max %.2f codes\n",
                    exact, size_t(W * H), maxUlps);
            }
        }
        if (!k.snormBits && !k.xr)
            return 0;
        // the code rules: the codes' mean rounded half up, half to even, half away from zero, toward zero
        const auto codeOf = [&](size_t texel, int c) -> int32_t {
            if (k.snormBits == 8)
                return int8_t(got[texel * k.bpp + c]);
            if (k.snormBits == 16) {
                int16_t x;
                memcpy(&x, &got[texel * k.bpp + 2 * c], 2);
                return x;
            }
            if (k.bpp == 4) {
                uint32_t w;
                memcpy(&w, &got[texel * 4], 4);
                return int32_t(w >> (10 * (2 - c)) & 1023);  // red at 20, green at 10, blue at 0
            }
            uint16_t f;  // BGRA10_XR: blue, green, red, alpha fields, the code in the top 10 bits
            const int field = c == 3 ? 3 : 2 - c;
            memcpy(&f, &got[texel * 8 + 2 * field], 2);
            return int32_t(f >> 6);
        };
        size_t rules[5] = {};
        size_t compared = 0;
        int shown = 0;
        for (size_t t = 0; t < W * H; t++)
            for (int c = 0; c < k.channels; c++) {
                int64_t sum = 0;
                for (uint32_t s = 0; s < samples; s++)
                    sum += codes[(t * samples + s) * 4 + c];
                const double exact = double(sum) / samples;
                const int32_t up = int32_t(std::floor(exact + 0.5)), even = int32_t(std::nearbyint(exact)),
                              away = int32_t(std::round(exact)), zero = int32_t(std::trunc(exact));
                const int32_t floatMean =
                    k.snormBits ? int32_t(std::nearbyint(double(mean[t * 4 + c]) * ((1 << (k.snormBits - 1)) - 1)))
                                : int32_t(std::nearbyint(double(mean[t * 4 + c]) * 510 + 384));
                const int32_t g = codeOf(t, c);
                rules[0] += g == up;
                rules[1] += g == even;
                rules[2] += g == away;
                rules[3] += g == zero;
                rules[4] += g == floatMean;
                compared++;
                if (g != up && g != even && g != away && shown++ < 6)
                    printf("  texel %zu channel %d: codes", t, c);
                if (g != up && g != even && g != away && shown <= 6 && k.bpp == 8) {
                    uint16_t f;
                    memcpy(&f, &got[t * 8 + 6], 2);
                    printf(" [alpha field %04x]", f);
                }
                if (g != up && g != even && g != away && shown <= 6) {
                    for (uint32_t s = 0; s < samples; s++)
                        printf(" %d", codes[(t * samples + s) * 4 + c]);
                    printf(" -> %d\n", g);
                }
            }
        if (k.xr && k.channels == 4) {
            size_t tiesDown = 0, floatRule = 0;
            for (size_t t = 0; t < W * H; t++) {
                int64_t sum = 0;
                float fsum = 0;
                for (uint32_t s = 0; s < samples; s++) {
                    const int32_t c = codes[(t * samples + s) * 4 + 3];
                    sum += c;
                    fsum += (float(c) - 384.0f) / 510.0f;
                }
                const int32_t g = codeOf(t, 3);
                tiesDown += g == int32_t((sum + samples / 2 - 1) / samples);
                floatRule += g == int32_t(std::nearbyint(fsum / float(samples) * 510.0f + 384.0f));
            }
            printf("alpha over %zu texels: ties down %zu, float mean %zu\n", size_t(W * H), tiesDown, floatRule);
            if (getenv("DUMP"))
                for (size_t t = 0; t < W * H; t++) {
                    printf("A");
                    for (uint32_t s = 0; s < samples; s++)
                        printf(" %d", codes[(t * samples + s) * 4 + 3]);
                    printf(" %d\n", codeOf(t, 3));
                }
        }
        printf("code rules over %zu channels: half up %zu, half even %zu, half away %zu, toward zero %zu, float mean "
               "nearest even %zu\n",
               compared, rules[0], rules[1], rules[2], rules[3], rules[4]);
    }
    return 0;
}
