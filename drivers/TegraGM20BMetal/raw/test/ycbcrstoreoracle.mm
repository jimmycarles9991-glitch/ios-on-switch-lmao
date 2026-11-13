// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr_store_rule.h against this Mac's Metal: render passes (clears, float draws, half draws) into two-plane
// YCbCr attachments over CoreVideo surfaces, every byte of both planes against the rule: the 4:2:0 and 4:2:2 code formats
// over their surfaces, the converted ones and their sRGB twins over both ranges under no matrix, BT.709 and BT.2020. The draws' floats lie beside the half midpoints and
// the ties of the 1/255, 1/1023, 1/2048 and 1/8192 grids, out of range and not numbers.
// Then the raw path's route on this Mac's GPU: the same texels in an RGBA32Float stand-in (a clear's colour as its half, as
// RawMetalEncode.mm puts it there), the pack's store states (raw/pack/ycbcr_store.metal) into R8/RG8 or R16/RG16 planes: their
// bytes must be Metal's too. And the packed states into R32Uint planes: their words must be their unpacked twins' codes,
// three to a word (this Mac's GPU faults on Metal's own passes into a packed target).
// usage: ycbcrstoreoracle <raw/pack directory>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <random>
#include <vector>

#include "../runtime/raw_ycbcr_store.h"

namespace {
constexpr uint32_t W = 128, H = 64, N = W * H;

const uint16_t kKnots[513] = {
#include "../pack/srgb_knots.h"
};

NSString *const kValues =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct V { float4 p [[position]]; };\n"
    @"vertex V strip(uint i [[vertex_id]]) { V v; v.p = float4(float(i & 1) * 2 - 1, float(i >> 1) * 2 - 1, 0, 1);"
    @" return v; }\n"
    @"fragment float4 floats(V in [[stage_in]], device const float4 *k [[buffer(0)]]) {\n"
    @"  return k[uint(in.p.y) * 128 + uint(in.p.x)]; }\n"
    @"fragment half4 halves(V in [[stage_in]], device const float4 *k [[buffer(0)]]) {\n"
    @"  return half4(k[uint(in.p.y) * 128 + uint(in.p.x)]); }\n";

float bitsOf(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

std::vector<float> valueSet(uint32_t seed)
{
    std::mt19937 rng(seed);
    auto beside = [&](double x, int spread) {
        float f = float(x);
        uint32_t u;
        memcpy(&u, &f, 4);
        return bitsOf(u + uint32_t(int(rng() % uint32_t(2 * spread + 1)) - spread));
    };
    const uint32_t grids[] = {255, 1023, 2048, 8192};
    std::vector<float> v(4 * N, 1.0f);
    for (uint32_t i = 0; i < N; i++)
        for (int c = 0; c < 3; c++) {
            const uint32_t k = rng();
            switch (k % 7) {
            case 0: {
                _Float16 a, b;
                const uint16_t h = uint16_t(k >> 8) % 0x3c00, h1 = h + 1;
                memcpy(&a, &h, 2);
                memcpy(&b, &h1, 2);
                v[4 * i + c] = beside((double(a) + double(b)) / 2, 1);
                break;
            }
            case 1:
            case 2: {
                const uint32_t g = grids[(k >> 3) % 4];
                v[4 * i + c] = beside(((k >> 8) % g + 0.5) / g, 3);
                break;
            }
            case 3: {
                const float special[] = {bitsOf(0x7fc00000u), INFINITY, -INFINITY, -0.0f, -0.3f, 1.7f, 0.0f, 1.0f};
                v[4 * i + c] = special[(k >> 8) % 8];
                break;
            }
            default:
                v[4 * i + c] = std::uniform_real_distribution<float>(-0.05f, 1.05f)(rng);
            }
        }
    return v;
}

struct Config {
    uint32_t format;
    OSType fourcc;
    const char *matrix;
};

bool fullRange(OSType f)
{
    return f == '420f' || f == '422f' || f == '444f' || f == 'xf20' || f == 'xf22' || f == 'xf44';
}

// The target's subsampling: its 4:2:0, 4:2:2 or 4:4:4 surface's
void halved(OSType f, bool &across, bool &down)
{
    across = f != '444v' && f != '444f' && f != 'x444' && f != 'xf44';
    down = f == '420v' || f == '420f' || f == 'x420' || f == 'xf20';
}

raw::YcbcrConvertedTarget convertedTarget(const Config &c)
{
    raw::YcbcrConvertedTarget t;
    t.bits = c.format < 505 || (c.format >= 520 && c.format < 525) ? 8 : 10;
    t.srgb = c.format >= 520;
    t.full = fullRange(c.fourcc);
    t.matrix = !c.matrix                          ? raw::YcbcrMatrix::Bt601
               : !strcmp(c.matrix, "ITU_R_709_2") ? raw::YcbcrMatrix::Bt709
                                                  : raw::YcbcrMatrix::Bt2020;
    halved(c.fourcc, t.across, t.down);
    return t;
}

raw::YcbcrStoreShape shapeOf(const Config &c)
{
    raw::YcbcrCodeTarget codes;
    if (!raw::ycbcrCodeTarget(c.format, codes))
        return raw::ycbcrConvertedShape(convertedTarget(c));
    return raw::ycbcrCodesShape(codes.max > 255 ? 10 : 8, true, codes.halvedDown);
}

struct Surface {
    CVPixelBufferRef pb = nullptr;
    IOSurfaceRef s = nullptr;
};

Surface makeSurface(const Config &c)
{
    Surface out;
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    if (!CVPixelBufferCreate(nullptr, W, H, c.fourcc, (__bridge CFDictionaryRef)attrs, &out.pb))
        out.s = CVPixelBufferGetIOSurface(out.pb);
    if (out.s && c.matrix)
        IOSurfaceSetValue(out.s, CFSTR("IOSurfaceYCbCrMatrix"), (__bridge CFStringRef) @(c.matrix));
    return out;
}

// Plane 0's codes then plane 1's, `bytes` a code (a 10-bit code its word's high bits)
std::vector<uint16_t> planes(IOSurfaceRef s, uint32_t bytes)
{
    std::vector<uint16_t> out;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    for (size_t p = 0; p < 2; p++) {
        const uint8_t *b = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, p));
        const size_t codes = p ? 2 * IOSurfaceGetWidthOfPlane(s, p) : IOSurfaceGetWidthOfPlane(s, p);
        for (size_t y = 0; y < IOSurfaceGetHeightOfPlane(s, p); y++)
            for (size_t x = 0; x < codes; x++) {
                const uint8_t *at = b + y * IOSurfaceGetBytesPerRowOfPlane(s, p) + x * bytes;
                out.push_back(bytes == 2 ? uint16_t((at[0] | at[1] << 8) >> 6) : at[0]);
            }
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return out;
}

void scribble(IOSurfaceRef s, uint32_t seed)
{
    IOSurfaceLock(s, 0, nullptr);
    for (size_t p = 0; p < 2; p++) {
        uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, p));
        for (size_t i = 0; i < IOSurfaceGetBytesPerRowOfPlane(s, p) * IOSurfaceGetHeightOfPlane(s, p); i++)
            b[i] = uint8_t(i * 31 + seed);
    }
    IOSurfaceUnlock(s, 0, nullptr);
}

std::vector<uint16_t> reference(const Config &c, const uint16_t *encode, const std::vector<float> &v,
                                raw::YcbcrTexelSource source)
{
    bool across, down;
    halved(c.fourcc, across, down);
    std::vector<uint16_t> luma(N), chroma(2 * (across ? W / 2 : W) * (down ? H / 2 : H));
    raw::YcbcrCodeTarget t;
    if (raw::ycbcrCodeTarget(c.format, t))
        raw::ycbcrStore(t, v.data(), W, H, source, luma.data(), chroma.data());
    else
        raw::ycbcrStoreConverted(convertedTarget(c), encode, v.data(), W, H, source, luma.data(), chroma.data());
    luma.insert(luma.end(), chroma.begin(), chroma.end());
    return luma;
}

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> lib, store;
    id<MTLBuffer> encode;  // pack/srgb_encode.h, at the _srgb states' buffer(1) as the device binds it

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

    // One pass into `target`: a clear of `clear` (no state) or a draw with `s` of `values` or the `sampled` texture
    void pass(id<MTLTexture> target, id<MTLRenderPipelineState> s, const float *values, MTLClearColor clear,
              id<MTLTexture> sampled = nil, const raw::YcbcrStoreShape *shape = nullptr)
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = s ? MTLLoadActionDontCare : MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = clear;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        if (s) {
            [e setRenderPipelineState:s];
            if (values)
                [e setFragmentBuffer:[dev newBufferWithBytes:values length:N * 16 options:MTLResourceStorageModeShared]
                              offset:0
                             atIndex:0];
            if (sampled)
                [e setFragmentTexture:sampled atIndex:0];
            if (shape)
                [e setFragmentBytes:shape length:sizeof *shape atIndex:0];
            if (shape && shape->srgb)
                [e setFragmentBuffer:encode offset:0 atIndex:raw::kSrgbEncodeSlot];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
};

// The pack's store states' bytes of `texels` (the stand-in's), plane 0's codes then plane 1's
std::vector<uint16_t> route(Gpu &g, const Config &c, const std::vector<float> &texels)
{
    const raw::YcbcrStoreShape shape = shapeOf(c);
    const bool wide = shape.max > 255;
    const uint32_t bytes = wide ? 2 : 1, cw = shape.across ? W / 2 : W, ch = shape.down ? H / 2 : H;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                  width:W
                                                                                 height:H
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> standIn = [g.dev newTextureWithDescriptor:td];
    [standIn replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:texels.data() bytesPerRow:W * 16];
    NSString *suffix = shape.srgb ? @"_srgb_frag" : @"_frag";
    std::vector<uint16_t> out;
    for (int plane = 0; plane < 2; plane++) {
        const MTLPixelFormat f = plane ? (wide ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm)
                                       : (wide ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm);
        td.pixelFormat = f;
        td.width = plane ? cw : W;
        td.height = plane ? ch : H;
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> t = [g.dev newTextureWithDescriptor:td];
        NSString *name =
            [NSString stringWithFormat:@"ycbcr_store_%@%u%@", plane ? @"chroma" : @"luma", wide ? 16 : 8, suffix];
        g.pass(t, g.state(g.store, @"ycbcr_store_vert", name, f), nullptr, {}, standIn, &shape);
        const uint32_t codes = uint32_t(td.width) * (plane ? 2 : 1), rows = uint32_t(td.height);
        std::vector<uint8_t> b(codes * bytes * rows);
        [t getBytes:b.data() bytesPerRow:codes * bytes fromRegion:MTLRegionMake2D(0, 0, td.width, rows) mipmapLevel:0];
        for (uint32_t i = 0; i < codes * rows; i++)
            out.push_back(wide ? uint16_t((b[2 * i] | b[2 * i + 1] << 8) >> 6) : b[i]);
    }
    return out;
}

// The packed states' words (raw/pack/ycbcr_store.metal's _packed functions) of `texels`, a packed target whose unpacked twin is
// `twin`: plane 0's words then plane 1's
std::vector<uint32_t> packedRoute(Gpu &g, const Config &twin, const std::vector<float> &texels)
{
    const raw::YcbcrStoreShape shape = raw::ycbcrPackedShape(shapeOf(twin));
    const uint32_t cw = shape.across ? W / 2 : W, ch = shape.down ? H / 2 : H;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                  width:W
                                                                                 height:H
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> standIn = [g.dev newTextureWithDescriptor:td];
    [standIn replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:texels.data() bytesPerRow:W * 16];
    std::vector<uint32_t> out;
    for (int plane = 0; plane < 2; plane++) {
        td.pixelFormat = MTLPixelFormatR32Uint;
        td.width = ((plane ? 2 * cw : W) + 2) / 3;
        td.height = plane ? ch : H;
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> t = [g.dev newTextureWithDescriptor:td];
        NSString *name = [NSString stringWithFormat:@"ycbcr_store_%@_packed%@", plane ? @"chroma" : @"luma",
                                                    shape.srgb ? @"_srgb_frag" : @"_frag"];
        g.pass(t, g.state(g.store, @"ycbcr_store_vert", name, MTLPixelFormatR32Uint), nullptr, {}, standIn, &shape);
        std::vector<uint32_t> words(td.width * td.height);
        [t getBytes:words.data()
            bytesPerRow:td.width * 4
             fromRegion:MTLRegionMake2D(0, 0, td.width, td.height)
            mipmapLevel:0];
        out.insert(out.end(), words.begin(), words.end());
    }
    return out;
}

// The rule's words of a packed target: its twin's codes (`codes`, plane 0's then plane 1's), three to a word a row
std::vector<uint32_t> packedWords(const Config &twin, const std::vector<uint16_t> &codes)
{
    bool across, down;
    halved(twin.fourcc, across, down);
    const uint32_t rowCodes[2] = {W, 2 * (across ? W / 2 : W)}, rows[2] = {H, down ? H / 2 : H};
    std::vector<uint32_t> out;
    size_t at = 0;
    for (int plane = 0; plane < 2; plane++)
        for (uint32_t y = 0; y < rows[plane]; y++) {
            std::vector<uint32_t> words((rowCodes[plane] + 2) / 3);
            raw::ycbcrPackRow(&codes[at], rowCodes[plane], words.data());
            out.insert(out.end(), words.begin(), words.end());
            at += rowCodes[plane];
        }
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: ycbcrstoreoracle <raw/pack directory>\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSError *e = nil;
        g.lib = [g.dev newLibraryWithSource:kValues options:nil error:&e];
        NSString *dir = @(argv[1]);
        NSString *src = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"ycbcr_store.metal"]
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
        std::vector<Config> configs = {{540, '420v', nullptr}, {540, '420f', nullptr}, {541, '422v', nullptr},
                                       {541, '422f', nullptr}, {543, 'x420', nullptr}, {543, 'xf20', nullptr},
                                       {544, 'x422', nullptr}, {544, 'xf22', nullptr}};
        const std::pair<uint32_t, OSType> converted[] = {
            {500, '420v'}, {500, '420f'}, {502, '422v'}, {502, '422f'}, {503, '444v'}, {503, '444f'},
            {505, 'x420'}, {505, 'xf20'}, {506, 'x422'}, {506, 'xf22'}, {507, 'x444'}, {507, 'xf44'},
            {520, '420v'}, {520, '420f'}, {522, '422v'}, {522, '422f'}, {523, '444v'}, {523, '444f'},
            {525, 'x420'}, {525, 'xf20'}, {526, 'x422'}, {526, 'xf22'}, {527, 'x444'}, {527, 'xf44'}};
        for (const auto &[format, fourcc] : converted)
            for (const char *matrix : {(const char *)nullptr, "ITU_R_709_2", "ITU_R_2020"})
                configs.push_back({format, fourcc, matrix});
        std::vector<std::vector<float>> sets;
        for (uint32_t k = 0; k < 6; k++)
            sets.push_back(valueSet(540 + k));
        uint32_t passes = 0, bad = 0, routeBad = 0;
        for (const Config &c : configs) {
            Surface s = makeSurface(c);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(c.format)
                                                                                         width:W
                                                                                        height:H
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> t = s.s ? [g.dev newTextureWithDescriptor:d iosurface:s.s plane:0] : nil;
            id<MTLRenderPipelineState> floats = g.state(g.lib, @"strip", @"floats", MTLPixelFormat(c.format)),
                                       halves = g.state(g.lib, @"strip", @"halves", MTLPixelFormat(c.format));
            const char fc[5] = {char(c.fourcc >> 24), char(c.fourcc >> 16), char(c.fourcc >> 8), char(c.fourcc), 0};
            if (!t || !floats || !halves) {
                printf("format %u over %s: not made\n", c.format, fc);
                bad++;
                continue;
            }
            const uint32_t bytes = shapeOf(c).max > 255 ? 2 : 1;
            uint32_t off = 0;
            auto check = [&](const std::vector<float> &v, raw::YcbcrTexelSource source,
                             const std::vector<float> &held) {
                const std::vector<uint16_t> want = reference(c, encode, v, source), got = planes(s.s, bytes),
                                            routed = route(g, c, held);
                passes++;
                if (got != want) {
                    if (off++ < 4)
                        printf("  %u over %s (%s) pass %u: a code off the rule\n", c.format, fc,
                               c.matrix ?: "no matrix", passes);
                    bad++;
                }
                routeBad += routed != got;
            };
            for (uint32_t k = 0; k < sets.size(); k++) {
                scribble(s.s, k);
                g.pass(t, floats, sets[k].data(), {});
                check(sets[k], raw::YcbcrTexelSource::DrawnFloat, sets[k]);
                std::vector<float> h(sets[k].size());
                for (size_t i = 0; i < h.size(); i++)
                    h[i] = float(_Float16(sets[k][i]));
                scribble(s.s, k + 7);
                g.pass(t, halves, sets[k].data(), {});
                check(h, raw::YcbcrTexelSource::DrawnHalf, h);
            }
            for (int i = 0; i <= 60; i++) {
                const double x = i / 55.0 - 0.04;
                const MTLClearColor k = MTLClearColorMake(x, 1.07 - x, std::fmod(x * 5.3, 1.0), 1);
                std::vector<float> v(4 * N), held(4 * N);
                for (uint32_t j = 0; j < N; j++)
                    for (int ch = 0; ch < 4; ch++) {
                        v[4 * j + ch] = float((&k.red)[ch]);
                        held[4 * j + ch] = float(_Float16(v[4 * j + ch]));
                    }
                scribble(s.s, uint32_t(i));
                g.pass(t, nil, nullptr, k);
                check(v, raw::YcbcrTexelSource::Clear, held);
            }
            CVPixelBufferRelease(s.pb);
        }
        // packed targets: Metal's own passes into them fault on this Mac (IOGPUCommandQueueErrorDomain 5), so their words are
        // held to their unpacked twins' measured codes, three to a word as Metal reads them
        const Config twins[] = {{505, 'x420', nullptr},      {506, 'x422', "ITU_R_709_2"}, {507, 'x444', nullptr},
                                {525, 'xf20', "ITU_R_2020"}, {526, 'x422', nullptr},       {527, 'xf44', "ITU_R_709_2"},
                                {543, 'x420', nullptr},      {544, 'xf22', nullptr}};
        uint32_t packedPasses = 0, packedBad = 0;
        for (const Config &twin : twins)
            for (uint32_t k = 0; k < sets.size(); k++) {
                const std::vector<uint32_t> want =
                    packedWords(twin, reference(twin, encode, sets[k], raw::YcbcrTexelSource::DrawnFloat));
                packedBad += packedRoute(g, twin, sets[k]) != want;
                packedPasses++;
            }
        printf(
            "%s: %u passes into two-plane YCbCr attachments, %u with a code off the rule, %u with a byte of the store "
            "states' off Metal's; %u of packed targets, %u with a word off their twins' codes\n",
            g.dev.name.UTF8String, passes, bad, routeBad, packedPasses, packedBad);
        return bad || routeBad || packedBad ? 1 : 0;
    }
}
