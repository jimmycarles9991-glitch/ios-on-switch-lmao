// generateMipmapsForTexture: of the chains the driver mips from words against this Mac's Metal: the 2D steps of
// R8Unorm_sRGB, A1BGR5Unorm, ABGR4Unorm and RGB9E5Float (format_standin.h formatMipState), the extended-range 2D
// steps and every 3D step (mip.h wordMipState, wordMip3dState). Each level Metal made, then the pack's state
// (pack/fmt.metal) on this Mac's GPU from the level above as Metal stored it, into the level's words; every word
// compared. RGB9E5Float's chains also of constructed words. Exit 1 when any word differs.
// usage: miporacle <pack directory>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "../format_standin.h"
#include "../mip.h"

namespace {

id<MTLDevice> dev;
id<MTLCommandQueue> queue;

struct Case {
    uint32_t format, view;
    bool volume;
    const char *state;
};

using Extent = std::array<uint32_t, 3>;

const char *kKinds[] = {"random", "two exponent clusters", "one exponent", "sparse", "near powers of two"};

std::string fileText(const std::string &path)
{
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// fmt.metal with its includes (the pack directory's tables) inlined, as newLibraryWithSource: has no include path
std::string packSource(const std::string &pack)
{
    std::string src = fileText(pack + "/fmt.metal"), out;
    const std::regex include("#include \"([\\w.]+)\"");
    std::sregex_iterator it(src.begin(), src.end(), include), end;
    size_t at = 0;
    for (; it != end; ++it) {
        out += src.substr(at, it->position() - at) + fileText(pack + "/" + (*it)[1].str());
        at = it->position() + it->length();
    }
    return out + src.substr(at);
}

uint32_t bytesOf(uint32_t view)
{
    return view == 13 ? 1 : view == 23 ? 2 : view == 113 ? 8 : 4;
}

void run(void (^body)(id<MTLCommandBuffer>))
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    body(cb);
    [cb commit];
    [cb waitUntilCompleted];
}

std::vector<uint8_t> level(id<MTLTexture> t, uint32_t l, uint32_t bytes)
{
    const NSUInteger w = std::max<NSUInteger>(1, t.width >> l), h = std::max<NSUInteger>(1, t.height >> l),
                     d = t.textureType == MTLTextureType3D ? std::max<NSUInteger>(1, t.depth >> l) : 1;
    id<MTLBuffer> b = [dev newBufferWithLength:w * h * d * bytes options:MTLResourceStorageModeShared];
    run(^(id<MTLCommandBuffer> cb) {
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:t
                         sourceSlice:0
                         sourceLevel:l
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, d)
                            toBuffer:b
                   destinationOffset:0
              destinationBytesPerRow:w * bytes
            destinationBytesPerImage:w * h * bytes];
        [e endEncoding];
    });
    const uint8_t *p = static_cast<const uint8_t *>(b.contents);
    return std::vector<uint8_t>(p, p + b.length);
}

// The largest difference of one field's code between two words of `format` (RGB9E5Float: of a channel's value, in the
// mantissa steps of the larger of the two shared exponents)
double fieldDiff(uint32_t format, uint64_t a, uint64_t b)
{
    double worst = 0;
    auto field = [&](int shift, int bits) {
        const int64_t x = int64_t(a >> shift & ((1ull << bits) - 1)), y = int64_t(b >> shift & ((1ull << bits) - 1));
        worst = std::max(worst, double(x > y ? x - y : y - x));
    };
    switch (format) {
    case 554:
    case 555:
        for (int k = 0; k < 3; k++)
            field(10 * k, 10);
        field(30, 2);
        break;
    case 552:
    case 553:
        for (int k = 0; k < 4; k++)
            field(16 * k + 6, 10);
        break;
    case gm20b::kR8UnormSrgb:
        field(0, 8);
        break;
    case gm20b::kA1Bgr5Unorm:
        field(0, 1);
        for (int k = 0; k < 3; k++)
            field(1 + 5 * k, 5);
        break;
    case gm20b::kAbgr4Unorm:
        for (int k = 0; k < 4; k++)
            field(4 * k, 4);
        break;
    case gm20b::kRgb9e5Float: {
        const int ea = int(a >> 27), eb = int(b >> 27), e = std::max(ea, eb);
        for (int k = 0; k < 3; k++)
            worst = std::max(worst, std::fabs(std::ldexp(double(a >> (9 * k) & 511), ea - e) -
                                              std::ldexp(double(b >> (9 * k) & 511), eb - e)));
        break;
    }
    }
    return worst;
}

// A level 0 word of `format`, of value kind `kind` (RGB9E5Float's alone are constructed)
uint64_t word(const Case &c, int kind, std::mt19937 &rng, const uint32_t clusters[3])
{
    const uint32_t a = rng(), b = rng();
    if (c.view == 113)
        return uint64_t(a & 1023) << 6 | uint64_t(a >> 10 & 1023) << 22 | uint64_t(a >> 20 & 1023) << 38 |
               uint64_t(b & 1023) << 54;
    switch (kind) {
    case 1:
        return (a & 0x7ffffff) | ((b >> 31) ? clusters[1] : clusters[0]) << 27;
    case 2:
        return (a & 0x7ffffff) | clusters[2] << 27;
    case 3:
        return b % 4 ? 0 : a;
    case 4: {
        const uint32_t near[] = {0, 1, 255, 256, 257, 511, 510, 128, 127, 384};
        return near[b % 10] | near[(b >> 4) % 10] << 9 | near[(b >> 8) % 10] << 18 | ((b >> 12) % 32) << 27;
    }
    default:
        return a;
    }
}

struct Tally {
    long steps = 0, off = 0;
    double most = 0;
};

// Every later level of a chain of `size` made by Metal against the state run on the level above Metal stored
void chain(const Case &c, id<MTLRenderPipelineState> state, const Extent &size, int kind, std::mt19937 &rng, Tally &t)
{
    const uint32_t w = size[0], h = size[1], d = size[2], bytes = bytesOf(c.view);
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = c.volume ? MTLTextureType3D : MTLTextureType2D;
    td.pixelFormat = MTLPixelFormat(c.format);
    td.width = w, td.height = h, td.depth = d;
    td.mipmapLevelCount = 1 + uint32_t(std::log2(double(std::max({w, h, d}))));
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    const uint32_t clusters[3] = {uint32_t(rng() % 12), 18 + uint32_t(rng() % 14), uint32_t(rng() % 32)};
    std::vector<uint8_t> top(size_t(w) * h * d * bytes);
    for (size_t i = 0; i < top.size(); i += bytes) {
        const uint64_t v = word(c, kind, rng, clusters);
        memcpy(&top[i], &v, bytes);
    }
    id<MTLBuffer> up = [dev newBufferWithBytes:top.data() length:top.size() options:MTLResourceStorageModeShared];
    run(^(id<MTLCommandBuffer> cb) {
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:up
                   sourceOffset:0
              sourceBytesPerRow:w * bytes
            sourceBytesPerImage:w * h * bytes
                     sourceSize:MTLSizeMake(w, h, d)
                      toTexture:tex
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [e generateMipmapsForTexture:tex];
        [e endEncoding];
    });
    for (uint32_t l = 0; l + 1 < tex.mipmapLevelCount; l++) {
        const uint32_t aw = std::max(1u, w >> l), ah = std::max(1u, h >> l), ad = c.volume ? std::max(1u, d >> l) : 1;
        const uint32_t lw = std::max(1u, aw / 2), lh = std::max(1u, ah / 2), ld = c.volume ? std::max(1u, ad / 2) : 1;
        const std::vector<uint8_t> above = level(tex, l, bytes), metal = level(tex, l + 1, bytes);
        MTLTextureDescriptor *vd = [MTLTextureDescriptor new];
        vd.textureType = c.volume ? MTLTextureType3D : MTLTextureType2D;
        vd.pixelFormat = MTLPixelFormat(c.view);
        vd.width = aw, vd.height = ah, vd.depth = ad;
        vd.storageMode = MTLStorageModeShared;
        vd.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> in = [dev newTextureWithDescriptor:vd];
        [in replaceRegion:MTLRegionMake3D(0, 0, 0, aw, ah, ad)
              mipmapLevel:0
                    slice:0
                withBytes:above.data()
              bytesPerRow:aw * bytes
            bytesPerImage:aw * ah * bytes];
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(c.view)
                                                                                      width:lw
                                                                                     height:lh
                                                                                  mipmapped:NO];
        od.storageMode = MTLStorageModeShared;
        od.usage = MTLTextureUsageRenderTarget;
        for (uint32_t z = 0; z < ld; z++) {
            id<MTLTexture> out = [dev newTextureWithDescriptor:od];
            run(^(id<MTLCommandBuffer> cb) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = out;
                rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:state];
                [re setFragmentTexture:in atIndex:0];
                const uint32_t step[8] = {aw, ah, lw, lh, ad, ld, z, 0};
                [re setFragmentBytes:step length:sizeof step atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [re endEncoding];
            });
            std::vector<uint8_t> ours(size_t(lw) * lh * bytes);
            [out getBytes:ours.data() bytesPerRow:lw * bytes fromRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:0];
            const uint8_t *m = metal.data() + size_t(z) * lw * lh * bytes;
            for (uint32_t i = 0; i < lw * lh; i++) {
                t.steps++;
                uint64_t a = 0, b = 0;
                memcpy(&a, &ours[i * bytes], bytes);
                memcpy(&b, m + i * bytes, bytes);
                if (a == b)
                    continue;
                t.off++;
                t.most = std::max(t.most, fieldDiff(c.format, a, b));
            }
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: miporacle <pack directory>\n");
            return 2;
        }
        dev = MTLCreateSystemDefaultDevice();
        queue = [dev newCommandQueue];
        MTLCompileOptions *exact = [MTLCompileOptions new];
        exact.mathMode = MTLMathModeSafe;
        NSError *e = nil;
        id<MTLLibrary> pack = [dev newLibraryWithSource:@(packSource(argv[1]).c_str()) options:exact error:&e];
        if (!pack) {
            fprintf(stderr, "library: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        std::vector<Case> cases;
        for (const uint32_t f : {gm20b::kR8UnormSrgb, gm20b::kA1Bgr5Unorm, gm20b::kAbgr4Unorm, gm20b::kRgb9e5Float})
            cases.push_back({f, gm20b::formatMipView(f), false, gm20b::formatMipState(f)});
        for (const uint32_t f : {552u, 553u, 554u, 555u})
            cases.push_back({f, gm20b::wordMipView(f), false, gm20b::wordMipState(f)});
        for (const uint32_t f : {gm20b::kR8UnormSrgb, gm20b::kA1Bgr5Unorm, gm20b::kAbgr4Unorm, gm20b::kRgb9e5Float,
                                 552u, 553u, 554u, 555u})
            cases.push_back({f, gm20b::wordMipView(f), true, gm20b::wordMip3dState(f)});
        const std::vector<Extent> flat = {{64, 64, 1},   {37, 23, 1}, {100, 7, 1}, {256, 128, 1},
                                          {301, 299, 1}, {255, 5, 1}, {5, 255, 1}, {1000, 9, 1}};
        const std::vector<Extent> deep = {{16, 16, 16}, {13, 7, 5},   {33, 9, 21},  {260, 3, 7},  {5, 259, 3},
                                          {64, 64, 32}, {33, 33, 33}, {127, 5, 9},  {9, 129, 17}, {255, 255, 5},
                                          {13, 7, 4},   {65, 33, 4},  {129, 17, 6}, {31, 63, 8},  {99, 45, 2},
                                          {9, 9, 9},    {1, 1, 7},    {2, 3, 11},   {31, 1, 15},  {16, 16, 5}};
        std::mt19937 rng(29);
        bool differs = false;
        long steps = 0, off = 0;
        for (const Case &c : cases) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [pack newFunctionWithName:@"fmt_vert"];
            pd.fragmentFunction = [pack newFunctionWithName:@(strrchr(c.state, '.') + 1)];
            pd.colorAttachments[0].pixelFormat = MTLPixelFormat(c.view);
            id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!state) {
                fprintf(stderr, "%s: %s\n", c.state, e.localizedDescription.UTF8String);
                return 1;
            }
            for (int kind = 0; kind < (c.format == gm20b::kRgb9e5Float ? 5 : 1); kind++) {
                Tally t;
                for (const Extent &s : c.volume ? deep : flat)
                    chain(c, state, s, kind, rng, t);
                printf("%-40s %8ld texels, %6ld differ, at most %.3g code (%s)\n", c.state, t.steps, t.off, t.most,
                       kKinds[kind]);
                differs = differs || t.off;
                steps += t.steps;
                off += t.off;
            }
        }
        printf("%s: %ld mip texels, %ld off Metal's; %s\n", dev.name.UTF8String, steps, off,
               differs ? "a state differs" : "every state word for word");
        return differs ? 1 : 0;
    }
}
