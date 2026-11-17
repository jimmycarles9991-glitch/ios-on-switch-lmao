// SPDX-License-Identifier: MIT
// Blended and channel-masked states into two-plane YCbCr attachments (RawMetalYcbcrTargets.mm standInTargetState) against this
// Mac's Metal: a clear and three draws a pass, every code of both planes. The raw path's route on this Mac's GPU: the same
// draws into an RGBA32Float stand-in cleared to the clear's halves, by the state made for it (its factors of the destination
// alpha as a destination alpha of 1 reads them: raw_standin_blend.h), then the pack's store states. A masked state's codes
// and a state blending by the destination alpha must be Metal's; a state blending by the source must lie within one code
// (Metal blends in halves of its own rounding, the stand-in in floats).
// usage: ycbcrblendoracle <raw/pack directory>
#include <cstdlib>
#include <random>

#include "../runtime/raw_standin_blend.h"
#include "ycbcr_oracle.h"

namespace {
using namespace ycbcr_oracle;

NSString *const kDraws = @"#include <metal_stdlib>\nusing namespace metal;\n"
                         @"struct V { float4 p [[position]]; };\n"
                         @"vertex V strip(uint i [[vertex_id]]) { V v; v.p = float4(float(i & 1) * 2 - 1, "
                         @"float(i >> 1) * 2 - 1, 0, 1); return v; }\n"
                         @"fragment float4 floats(V in [[stage_in]], device const float4 *k [[buffer(0)]]) {\n"
                         @"  return k[uint(in.p.y) * 128 + uint(in.p.x)]; }\n";

struct Case {
    const char *name;
    bool blending;
    MTLBlendFactor source, destination;
    MTLColorWriteMask mask;
    double clearAlpha;
    bool exact;
};

const Case kCases[] = {
    {"over", true, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLColorWriteMaskAll, 1, false},
    {"add", true, MTLBlendFactorOne, MTLBlendFactorOne, MTLColorWriteMaskAll, 1, false},
    {"dst_alpha", true, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha, MTLColorWriteMaskAll,
     0.25, true},
    {"saturated", true, MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorOne, MTLColorWriteMaskAll, 0.25, true},
    {"mask_luma", false, MTLBlendFactorOne, MTLBlendFactorZero, MTLColorWriteMaskGreen, 1, true},
    {"mask_chroma", false, MTLBlendFactorOne, MTLBlendFactorZero, MTLColorWriteMaskRed | MTLColorWriteMaskBlue, 1,
     true},
    {"over_mask_luma", true, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLColorWriteMaskGreen, 1,
     false},
};

id<MTLRenderPipelineState> drawState(Gpu &g, MTLPixelFormat format, const Case &c, bool standIn)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g.lib newFunctionWithName:@"strip"];
    d.fragmentFunction = [g.lib newFunctionWithName:@"floats"];
    MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[0];
    a.pixelFormat = standIn ? MTLPixelFormat(raw::kYcbcrTargetStandIn) : format;
    a.blendingEnabled = c.blending;
    a.writeMask = c.mask;
    a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = c.source;
    a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = c.destination;
    if (standIn) {
        a.sourceRGBBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(c.source), true));
        a.destinationRGBBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(c.destination), true));
        a.sourceAlphaBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(c.source), false));
        a.destinationAlphaBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(c.destination), false));
    }
    NSError *e = nil;
    id<MTLRenderPipelineState> s = [g.dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!s)
        fprintf(stderr, "state %s: %s\n", c.name, e.localizedDescription.UTF8String);
    return s;
}

void drawPass(Gpu &g, id<MTLTexture> target, id<MTLRenderPipelineState> s, const std::vector<id<MTLBuffer>> &draws,
              MTLClearColor clear)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = clear;
    id<MTLCommandBuffer> cb = [g.q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:s];
    for (id<MTLBuffer> b : draws) {
        [e setFragmentBuffer:b offset:0 atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
}

double half(double v)
{
    return double(_Float16(float(v)));
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: ycbcrblendoracle <raw/pack directory>\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSError *e = nil;
        g.lib = [g.dev newLibraryWithSource:kDraws options:nil error:&e];
        NSString *src =
            [NSString stringWithContentsOfFile:[@(argv[1]) stringByAppendingPathComponent:@"ycbcr_store.metal"]
                                      encoding:NSUTF8StringEncoding
                                         error:nil];
        MTLCompileOptions *exact = [MTLCompileOptions new];
        exact.mathMode = MTLMathModeSafe;
        g.store = src ? [g.dev newLibraryWithSource:src options:exact error:&e] : nil;
        if (!g.lib || !g.store) {
            fprintf(stderr, "libraries: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        uint16_t encode[raw::kYcbcrSrgbSteps + 1];
        raw::ycbcrSrgbEncodeTable(kKnots, encode);
        g.encode = [g.dev newBufferWithBytes:encode length:sizeof encode options:MTLResourceStorageModeShared];
        const Config configs[] = {
            {540, '420v', nullptr}, {541, '422f', nullptr},       {543, 'x420', nullptr},       {544, 'xf22', nullptr},
            {500, '420v', nullptr}, {500, '420f', "ITU_R_709_2"}, {502, '422v', "ITU_R_2020"},  {505, 'x420', nullptr},
            {507, 'xf44', nullptr}, {520, '420v', nullptr},       {525, 'xf20', "ITU_R_709_2"},
        };
        std::mt19937 rng(6);
        std::uniform_real_distribution<float> u(0, 1);
        MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                      width:W
                                                                                     height:H
                                                                                  mipmapped:NO];
        sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        sd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> standIn = [g.dev newTextureWithDescriptor:sd];
        uint32_t passes = 0, failed = 0, exactOff = 0, beyondOne = 0;
        uint64_t oneOff = 0, codes = 0;
        for (const Config &c : configs) {
            Surface s = makeSurface(c);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(c.format)
                                                                                         width:W
                                                                                        height:H
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> t = s.s ? [g.dev newTextureWithDescriptor:d iosurface:s.s plane:0] : nil;
            const char fc[5] = {char(c.fourcc >> 24), char(c.fourcc >> 16), char(c.fourcc >> 8), char(c.fourcc), 0};
            if (!t) {
                printf("format %u over %s: not made\n", c.format, fc);
                failed++;
                continue;
            }
            const uint32_t bytes = shapeOf(c).max > 255 ? 2 : 1;
            for (const Case &k : kCases) {
                id<MTLRenderPipelineState> metal = drawState(g, MTLPixelFormat(c.format), k, false),
                                           route = drawState(g, MTLPixelFormat(c.format), k, true);
                if (!metal || !route) {
                    failed++;
                    continue;
                }
                std::vector<id<MTLBuffer>> draws;
                for (int n = 0; n < 3; n++) {
                    std::vector<float> v(4 * N);
                    for (float &x : v)
                        x = !strcmp(k.name, "add") ? u(rng) * 0.4f : u(rng);
                    draws.push_back([g.dev newBufferWithBytes:v.data()
                                                       length:v.size() * 4
                                                      options:MTLResourceStorageModeShared]);
                }
                const MTLClearColor clear = MTLClearColorMake(0.31, 0.52, 0.73, k.clearAlpha);
                scribble(s.s, passes);
                drawPass(g, t, metal, draws, clear);
                drawPass(g, standIn, route, draws,
                         MTLClearColorMake(half(clear.red), half(clear.green), half(clear.blue), half(clear.alpha)));
                const std::vector<uint16_t> got = planes(s.s, bytes), routed = routeOf(g, c, standIn);
                uint32_t off = 0, most = 0;
                for (size_t i = 0; i < got.size(); i++) {
                    const uint32_t d1 = uint32_t(std::abs(int(got[i]) - int(routed[i])));
                    off += d1 != 0;
                    most = std::max(most, d1);
                }
                passes++;
                codes += got.size();
                if (k.exact) {
                    exactOff += off != 0;
                } else {
                    oneOff += off;
                    beyondOne += most > 1;
                }
                if (off && (k.exact || most > 1))
                    printf("  %u over %s (%s) %s: %u of %zu codes off, at most %u\n", c.format, fc,
                           c.matrix ?: "no matrix", k.name, off, got.size(), most);
            }
            CVPixelBufferRelease(s.pb);
        }
        printf(
            "%s: %u passes of blended and masked states into two-plane YCbCr attachments; %u not made; %u of the "
            "masked and destination-alpha ones with a code off the route's; %u blended ones with a code more than one "
            "off; %llu codes one off of %llu\n",
            g.dev.name.UTF8String, passes, failed, exactOff, beyondOne, (unsigned long long)oneOff,
            (unsigned long long)codes);
        return failed || exactOff || beyondOne ? 1 : 0;
    }
}
