// SPDX-License-Identifier: MIT
// raw GM20B path: pack/mip.metal's 3D steps (mip_3d_*_frag), run on this Mac's own GPU a plane a pass as the driver draws
// them, against this Mac's Metal generateMipmapsForTexture: of the same 3D texture, every level, even and odd sizes. Metal's
// step is the texture unit's trilinear tap, whose weights the state takes exactly (1/256 steps) but whose blend no fp32 order
// of the eight texels reproduces bit for bit: a unorm or sRGB channel must be within one code of Metal's, the float channels'
// largest difference (in units of their last place) is printed. usage: mip3doracle <pack/mip.metal>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

struct Field {
    uint32_t at, bits;
};

struct Case {
    MTLPixelFormat format;
    const char *function;
    NSUInteger texel;
    std::vector<Field> fields;  // each channel's bits in the texel
    bool unorm;
};

std::vector<Field> whole(NSUInteger channels, uint32_t bits)
{
    std::vector<Field> f;
    for (uint32_t c = 0; c < channels; c++)
        f.push_back({c * bits, bits});
    return f;
}

const std::vector<Case> &cases()
{
    static const std::vector<Case> c = {
        {MTLPixelFormatA8Unorm, "mip_3d_a8_frag", 1, whole(1, 8), true},
        {MTLPixelFormatR8Unorm, "mip_3d_r8_frag", 1, whole(1, 8), true},
        {MTLPixelFormatR16Unorm, "mip_3d_r16_frag", 2, whole(1, 16), true},
        {MTLPixelFormatR16Float, "mip_3d_r16f_frag", 2, whole(1, 16), false},
        {MTLPixelFormatRG8Unorm, "mip_3d_rg8_frag", 2, whole(2, 8), true},
        {MTLPixelFormatB5G6R5Unorm, "mip_3d_b5g6r5_frag", 2, {{0, 5}, {5, 6}, {11, 5}}, true},
        {MTLPixelFormatBGR5A1Unorm, "mip_3d_bgr5a1_frag", 2, {{0, 5}, {5, 5}, {10, 5}, {15, 1}}, true},
        {MTLPixelFormatR32Float, "mip_3d_r32f_frag", 4, whole(1, 32), false},
        {MTLPixelFormatRG16Unorm, "mip_3d_rg16_frag", 4, whole(2, 16), true},
        {MTLPixelFormatRG16Float, "mip_3d_rg16f_frag", 4, whole(2, 16), false},
        {MTLPixelFormatRGBA8Unorm, "mip_3d_rgba8_frag", 4, whole(4, 8), true},
        {MTLPixelFormatBGRA8Unorm, "mip_3d_bgra8_frag", 4, whole(4, 8), true},
        {MTLPixelFormatRGBA8Unorm_sRGB, "mip_3d_rgba8_srgb_frag", 4, whole(4, 8), true},
        {MTLPixelFormatBGRA8Unorm_sRGB, "mip_3d_bgra8_srgb_frag", 4, whole(4, 8), true},
        {MTLPixelFormatRGB10A2Unorm, "mip_3d_rgb10a2_frag", 4, {{0, 10}, {10, 10}, {20, 10}, {30, 2}}, true},
        {MTLPixelFormatRG11B10Float, "mip_3d_rg11b10f_frag", 4, {{0, 11}, {11, 11}, {22, 10}}, false},
        {MTLPixelFormatBGR10A2Unorm, "mip_3d_bgr10a2_frag", 4, {{0, 10}, {10, 10}, {20, 10}, {30, 2}}, true},
        {MTLPixelFormatRG32Float, "mip_3d_rg32f_frag", 8, whole(2, 32), false},
        {MTLPixelFormatRGBA16Unorm, "mip_3d_rgba16_frag", 8, whole(4, 16), true},
        {MTLPixelFormatRGBA16Float, "mip_3d_rgba16f_frag", 8, whole(4, 16), false},
        {MTLPixelFormatRGBA32Float, "mip_3d_rgba32f_frag", 16, whole(4, 32), false},
    };
    return c;
}

id<MTLTexture> volume(id<MTLDevice> dev, MTLPixelFormat f, NSUInteger w, NSUInteger h, NSUInteger d, NSUInteger levels)
{
    MTLTextureDescriptor *t = [MTLTextureDescriptor new];
    t.textureType = MTLTextureType3D;
    t.pixelFormat = f;
    t.width = w;
    t.height = h;
    t.depth = d;
    t.mipmapLevelCount = levels;
    t.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    t.storageMode = MTLStorageModeShared;
    return [dev newTextureWithDescriptor:t];
}

std::vector<uint8_t> levelBytes(id<MTLTexture> t, NSUInteger level, NSUInteger texel)
{
    const NSUInteger w = std::max<NSUInteger>(1, t.width >> level), h = std::max<NSUInteger>(1, t.height >> level),
                     d = std::max<NSUInteger>(1, t.depth >> level);
    std::vector<uint8_t> out(w * h * d * texel);
    [t getBytes:out.data()
          bytesPerRow:w * texel
        bytesPerImage:w * h * texel
           fromRegion:MTLRegionMake3D(0, 0, 0, w, h, d)
          mipmapLevel:level
                slice:0];
    return out;
}

// Level 0's bytes: any value, a half's exponent kept finite, a float's modest, RG11B10Float's channels finite
std::vector<uint8_t> pattern(const Case &c, size_t count)
{
    std::vector<uint8_t> b(count * c.texel);
    uint32_t seed = 0x9e3779b9u;
    for (uint8_t &x : b) {
        seed = seed * 1664525u + 1013904223u;
        x = uint8_t(seed >> 24);
    }
    if (c.format == MTLPixelFormatR16Float || c.format == MTLPixelFormatRG16Float ||
        c.format == MTLPixelFormatRGBA16Float)
        for (size_t i = 1; i < b.size(); i += 2)
            b[i] &= 0x7b;
    if (c.format == MTLPixelFormatR32Float || c.format == MTLPixelFormatRG32Float ||
        c.format == MTLPixelFormatRGBA32Float)
        for (size_t i = 3; i < b.size(); i += 4)
            b[i] = uint8_t(0x40 | (b[i] & 0x81));
    if (c.format == MTLPixelFormatRG11B10Float)
        for (size_t i = 0; i < b.size(); i += 4) {
            b[i + 1] &= 0xbf;
            b[i + 3] &= 0x77;
        }
    return b;
}

MTLPixelFormat codesFormat(MTLPixelFormat f)
{
    return f == MTLPixelFormatRGBA8Unorm_sRGB   ? MTLPixelFormatRGBA8Unorm
           : f == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm
                                                : f;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSString *source =
            argc > 1 ? [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil] : nil;
        if (!dev || !source)
            return 2;
        MTLCompileOptions *o = [MTLCompileOptions new];
        o.mathMode = MTLMathModeSafe;
        NSError *e = nil;
        id<MTLLibrary> mips = [dev newLibraryWithSource:source options:o error:&e];
        if (!mips) {
            fprintf(stderr, "mip.metal: %s\n", e.localizedDescription.UTF8String);
            return 2;
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        unsigned failed = 0;
        for (const Case &c : cases())
            for (const auto [w, h, d] : {std::tuple<NSUInteger, NSUInteger, NSUInteger>{16, 16, 16}, {13, 7, 5}}) {
                const NSUInteger levels = 1 + NSUInteger(std::log2(double(std::max({w, h, d}))));
                id<MTLTexture> metal = volume(dev, c.format, w, h, d, levels),
                               ours = volume(dev, c.format, w, h, d, levels);
                MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
                pd.vertexFunction = [mips newFunctionWithName:@"mip_vert"];
                pd.fragmentFunction = [mips newFunctionWithName:@(c.function)];
                pd.colorAttachments[0].pixelFormat = c.format;
                id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
                if (!metal || !ours || !state) {
                    printf("%s: no texture or state\n", c.function);
                    failed++;
                    continue;
                }
                const std::vector<uint8_t> bytes = pattern(c, w * h * d);
                for (id<MTLTexture> t : {metal, ours})
                    [t replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d)
                          mipmapLevel:0
                                slice:0
                            withBytes:bytes.data()
                          bytesPerRow:w * c.texel
                        bytesPerImage:w * h * c.texel];
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b generateMipmapsForTexture:metal];
                [b endEncoding];
                for (NSUInteger l = 0; l + 1 < levels; l++) {
                    id<MTLTexture> above = [ours newTextureViewWithPixelFormat:codesFormat(c.format)
                                                                   textureType:MTLTextureType3D
                                                                        levels:NSMakeRange(l, 1)
                                                                        slices:NSMakeRange(0, 1)];
                    const uint32_t step[8] = {uint32_t(std::max<NSUInteger>(1, w >> l)),
                                              uint32_t(std::max<NSUInteger>(1, h >> l)),
                                              uint32_t(std::max<NSUInteger>(1, w >> (l + 1))),
                                              uint32_t(std::max<NSUInteger>(1, h >> (l + 1))),
                                              uint32_t(std::max<NSUInteger>(1, d >> l)),
                                              uint32_t(std::max<NSUInteger>(1, d >> (l + 1))),
                                              0,
                                              0};
                    for (uint32_t z = 0; z < step[5]; z++) {
                        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                        p.colorAttachments[0].texture = ours;
                        p.colorAttachments[0].level = l + 1;
                        p.colorAttachments[0].depthPlane = z;
                        p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                        p.colorAttachments[0].storeAction = MTLStoreActionStore;
                        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:p];
                        [enc setRenderPipelineState:state];
                        [enc setFragmentTexture:above atIndex:0];
                        uint32_t plane[8];
                        std::copy(step, step + 8, plane);
                        plane[6] = z;
                        [enc setFragmentBytes:plane length:sizeof plane atIndex:0];
                        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                        [enc endEncoding];
                    }
                }
                [cb commit];
                [cb waitUntilCompleted];
                size_t differ = 0, total = 0;
                uint64_t largest = 0;
                for (NSUInteger l = 1; l < levels; l++) {
                    const std::vector<uint8_t> want = levelBytes(metal, l, c.texel), got = levelBytes(ours, l, c.texel);
                    for (size_t i = 0; i < want.size(); i += c.texel) {
                        total++;
                        if (!memcmp(&want[i], &got[i], c.texel))
                            continue;
                        differ++;
                        for (const Field &f : c.fields) {
                            uint64_t x = 0, y = 0;
                            for (uint32_t bit = 0; bit < f.bits; bit++) {
                                const uint32_t at = f.at + bit;
                                x |= uint64_t(want[i + at / 8] >> (at % 8) & 1) << bit;
                                y |= uint64_t(got[i + at / 8] >> (at % 8) & 1) << bit;
                            }
                            largest = std::max(largest, x > y ? x - y : y - x);
                        }
                    }
                }
                printf("%-24s %lux%lux%lu: %zu of %zu texels differ from Metal's mips, by at most %llu %s\n",
                       c.function, (unsigned long)w, (unsigned long)h, (unsigned long)d, differ, total,
                       (unsigned long long)largest, c.unorm ? "code(s)" : "unit(s) in the last place");
                failed += c.unorm && largest > 1;
            }
        printf("mip3doracle: %u case(s) failed\n", failed);
        return failed ? 1 : 0;
    }
}
