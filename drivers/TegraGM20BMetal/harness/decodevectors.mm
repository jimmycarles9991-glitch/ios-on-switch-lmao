// The host test's vectors (test/decode_vectors.h): blocks and their texels as this Mac's GPU samples them, in the
// decoders' terms (pack/astc_texel.h forms: 8-bit codes, an sRGB format's read back through this Mac's sRGB table;
// 12-bit codes; half bits). ASTC blocks of every block mode a footprint takes, one block of each kind they exercise (void
// extent, partition count, dual plane, colour endpoint mode, the error colour), two texels each; then HDR RGB blocks
// whose signed offsets are negative and reach the texel, one for each mode that scales them; PVRTC levels whole.
// usage: decodevectors <out header>, then clang-format it
//   build: clang++ -std=c++17 -fobjc-arc -I pack decodevectors.mm -framework Foundation -framework Metal
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>
#include "astc_decode.h"

using namespace gm20b::decode;

static id<MTLDevice> gD;
static id<MTLCommandQueue> gQ;
static id<MTLComputePipelineState> gGet;
static float gSrgb[256];

static std::vector<float> readBack(id<MTLTexture> t)
{
    id<MTLBuffer> o = [gD newBufferWithLength:t.width * t.height * 16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [gQ commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:gGet];
    [c setTexture:t atIndex:0];
    [c setBuffer:o offset:0 atIndex:0];
    [c dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
    [c endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    std::vector<float> v(t.width * t.height * 4);
    memcpy(v.data(), o.contents, v.size() * 4);
    return v;
}

static id<MTLTexture> upload(uint32_t f, uint32_t w, uint32_t h, const void *bytes, size_t row)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)f
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [gD newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bytes bytesPerRow:row];
    return t;
}

// A channel as the decoder gives it, from what the GPU read
static uint32_t asDecoded(float v, int form, bool srgb, bool colour)
{
    if (form == kAstcHalf) {
        if (std::isnan(v))
            return 0xffff;
        _Float16 h = (_Float16)v;
        uint16_t b;
        memcpy(&b, &h, 2);
        return b;
    }
    if (srgb && colour) {
        for (uint32_t k = 0; k < 256; k++)
            if (gSrgb[k] == v)
                return k;
        return 0xdead;
    }
    const float scale = form == kAstcUnorm12 ? 4095.0f : 255.0f;
    const uint32_t k = uint32_t(std::lround(v * scale));
    return float(k) / scale == v ? k : 0xdead;
}

static std::string bucket(Words4 b, int bw, int bh, int form)
{
    const AstcMode m = astcMode(b, bw, bh);
    if (m.voidExtent)
        return (b.w0 >> 9) & 1 ? "void-hdr" : "void-ldr";
    const Texel t = astcTexel(b, bw, bh, 0, 0, form);
    const Texel e = astcError(form);
    if (t.r == e.r && t.g == e.g && t.b == e.b && t.a == e.a)
        return "error";
    int cem = m.cem;
    if (m.partitions > 1)
        cem = (m.cem & 3) == 0 ? m.cem >> 2 : -1;
    return "p" + std::to_string(m.partitions) + (m.dual ? "d" : "") + "c" + std::to_string(cem);
}

static std::vector<uint32_t> blockModes(uint32_t bw, uint32_t bh)
{
    std::vector<uint32_t> modes;
    for (uint32_t m = 0; m < 2048; m++) {
        const Words4 b = {m, 0, 0, 0};
        const AstcMode a = astcMode(b, int(bw), int(bh));
        const Quant q = weightQuant(a.weightRange);
        const int n = a.gridW * a.gridH * (a.dual ? 2 : 1), bits = iseBits(q, n);
        if (!a.voidExtent && !a.error && !reservedQuant(q) && n <= 64 && bits >= 24 && bits <= 96)
            modes.push_back(m);
    }
    return modes;
}

// What a one-partition, one-plane HDR RGB direct block (colour endpoint modes 11, 14, 15) multiplies by a power of two
// and is negative at texel (x, y): hdrRgbDirect's vd0 or vd1, hdrAlpha's v7. Each only where it reaches the texel: its
// endpoint strictly inside 0..0xfff and the texel's weight not all on the other endpoint.
static std::vector<std::string> negativeOffsets(Words4 b, int bw, int bh, int x, int y)
{
    const AstcMode m = astcMode(b, bw, bh);
    if (m.voidExtent || m.error || m.partitions != 1 || m.dual || (m.cem != 11 && m.cem < 14))
        return {};
    const Quant wq = weightQuant(m.weightRange);
    const int weightBits = iseBits(wq, m.gridW * m.gridH), count = 2 * ((m.cem >> 2) + 1);
    const Quant q = endpointQuant(count, maxInt(128 - m.configBits - weightBits, 0));
    if (reservedQuant(q))
        return {};
    const Words4 ends = lowBits(b, m.primaryBits + iseBits(q, count));
    int u[8] = {};
    for (int i = 0; i < count; i++)
        u[i] = unquantizeEndpoint(iseValue(ends, m.primaryBits, i, q), q);
    const Values8 v = {u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]};
    const Endpoints e = endpointsOfMode(m.cem, v);
    const int w = astcWeight(reversedLow(b, weightBits), m, wq, bw, bh, x, y, 0, 1);
    const auto inside = [](int c) {
        return c > 0 && c < 0xfff;
    };
    std::vector<std::string> found;
    const int major = ((v.v4 & 0x80) >> 7) | ((v.v5 & 0x80) >> 6);
    const int mode = ((v.v1 & 0x80) >> 7) | ((v.v2 & 0x80) >> 6) | ((v.v3 & 0x80) >> 5);
    const int dBits = 7 - (mode & 1) - ((mode & 5) == 4 ? 2 : 0), shift = (mode >> 1) ^ 3;
    const bool d0 = (v.v4 >> (dBits - 1)) & 1, d1 = (v.v5 >> (dBits - 1)) & 1;
    const int g0 = major == 1 ? e.e0.r : e.e0.g, b0 = major == 2 ? e.e0.r : e.e0.b;
    if (major != 3 && shift && w < 64 && ((d0 && inside(g0)) || (d1 && inside(b0))))
        found.push_back("negative d, mode " + std::to_string(mode));
    const int alphaMode = ((v.v6 >> 7) & 1) | ((v.v7 >> 6) & 2);
    if (m.cem == 15 && alphaMode != 3 && ((v.v7 >> (5 - alphaMode)) & 1) && w > 0 && inside(e.e1.a))
        found.push_back("negative alpha offset, mode " + std::to_string(alphaMode));
    return found;
}

static void printAstc(FILE *out, uint32_t f, uint32_t bw, uint32_t bh, Words4 b, uint32_t x, uint32_t y, const float *p,
                      int form, bool srgb, const std::string &what)
{
    fprintf(out,
            "    {%u, %u, %u, {0x%08x, 0x%08x, 0x%08x, 0x%08x}, %u, %u, {0x%x, 0x%x, 0x%x, 0x%x}, "
            "\"%s\"},\n",
            f, bw, bh, b.w0, b.w1, b.w2, b.w3, x, y, asDecoded(p[0], form, srgb, true),
            asDecoded(p[1], form, srgb, true), asDecoded(p[2], form, srgb, true), asDecoded(p[3], form, srgb, false),
            what.c_str());
}

int main(int, char **argv)
{
    @autoreleasepool {
        gD = MTLCreateSystemDefaultDevice();
        gQ = [gD newCommandQueue];
        NSString *src =
            @"#include <metal_stdlib>\nusing namespace metal;\nkernel void get(texture2d<float, access::read> t "
            @"[[texture(0)]], device float4 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { if (p.x < "
            @"t.get_width() && p.y < t.get_height()) o[p.y * t.get_width() + p.x] = t.read(p); }\n";
        gGet = [gD newComputePipelineStateWithFunction:[[gD newLibraryWithSource:src options:nil
                                                                           error:nil] newFunctionWithName:@"get"]
                                                 error:nil];
        {
            uint8_t px[1024];
            for (int i = 0; i < 256; i++)
                px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = uint8_t(i), px[4 * i + 3] = 255;
            id<MTLTexture> t = upload(71, 256, 1, px, 1024);
            const auto v = readBack(t);
            for (int i = 0; i < 256; i++)
                gSrgb[i] = v[4 * i];
        }
        FILE *out = fopen(argv[1], "w");
        fprintf(out, "// SPDX-License-Identifier: MIT\n// Generated by harness/decodevectors.mm on an M4 Max: blocks "
                     "and what its GPU samples from them, in the\n// decoders' terms (pack/astc_texel.h forms, "
                     "pvrtc_decode.h codes).\n#pragma once\n#include <cstdint>\n\n");
        fprintf(out, "struct AstcVector {\n    uint32_t format, blockW, blockH, words[4], x, y;\n    uint32_t "
                     "want[4];\n    const char *exercises;\n};\n\n");
        fprintf(out, "inline constexpr AstcVector kAstcVectors[] = {\n");
        std::mt19937 rng(2026);

        struct F {
            uint32_t f, bw, bh;
        };

        for (const F f : {F{185, 4, 2}, F{191, 8, 4}, F{203, 4, 2}, F{209, 8, 4}, F{186, 4, 4}, F{204, 4, 4},
                          F{221, 4, 2}, F{222, 4, 4}, F{227, 8, 4}, F{236, 12, 12}}) {
            const bool srgb = f.f <= 200, hdr = f.f >= 221;
            const int form = hdr ? kAstcHalf : srgb ? kAstcUnorm8 : kAstcUnorm12;
            const std::vector<uint32_t> modes = blockModes(f.bw, f.bh);
            const uint32_t count = 64 * 64;
            std::vector<uint32_t> words(count * 4);
            for (auto &w : words)
                w = rng();
            for (uint32_t k = 0; k < count; k++) {
                uint32_t *w = &words[k * 4];
                if (k % 32 == 0) {  // void extent, half with an HDR colour, extents all ones; a few colours near zero
                    w[0] = 0xfffffdfcu | (k % 64 ? 0x200 : 0);
                    w[1] = 0xffffffffu;
                    if (k % 96 == 0)
                        w[2] = (w[2] & 0xffff0000u) | (k % 3), w[3] &= 0xffff0003u;
                } else {
                    w[0] = (w[0] & ~0x7ffu) | modes[rng() % modes.size()];
                }
            }
            id<MTLTexture> t = upload(f.f, 64 * f.bw, 64 * f.bh, words.data(), 64 * 16);
            const auto v = readBack(t);
            std::map<std::string, int> taken;
            for (uint32_t k = 0; k < count; k++) {
                const Words4 b = {words[k * 4], words[k * 4 + 1], words[k * 4 + 2], words[k * 4 + 3]};
                const std::string what = bucket(b, int(f.bw), int(f.bh), form);
                if (taken[what]++ >= 1)
                    continue;
                const uint32_t bx = k % 64, by = k / 64;
                for (const uint32_t corner : {0u, 1u}) {
                    const uint32_t x = corner ? f.bw - 1 : 0, y = corner ? f.bh - 1 : 0;
                    const float *p = &v[((by * f.bh + y) * 64 * f.bw + bx * f.bw + x) * 4];
                    printAstc(out, f.f, f.bw, f.bh, b, x, y, p, form, srgb, what);
                }
            }
        }
        // An engine of its own, so the blocks above and the PVRTC levels below stay what they were
        std::mt19937 offsets(1121);
        for (const F f : {F{221, 4, 2}, F{222, 4, 4}, F{227, 8, 4}, F{236, 12, 12}}) {
            const std::vector<uint32_t> modes = blockModes(f.bw, f.bh);
            const uint32_t count = 64 * 64, cems[3] = {11, 14, 15};
            std::vector<uint32_t> words(count * 4);
            for (auto &w : words)
                w = offsets();
            for (uint32_t k = 0; k < count; k++) {
                const uint32_t cem = cems[offsets() % 3], mode = modes[offsets() % modes.size()];
                words[k * 4] = (words[k * 4] & ~0x1ffffu) | cem << 13 | mode;
            }
            id<MTLTexture> t = upload(f.f, 64 * f.bw, 64 * f.bh, words.data(), 64 * 16);
            const auto v = readBack(t);
            std::map<std::string, int> taken;
            for (uint32_t k = 0; k < count; k++) {
                const Words4 b = {words[k * 4], words[k * 4 + 1], words[k * 4 + 2], words[k * 4 + 3]};
                std::string what;
                bool fresh = false;
                for (const uint32_t corner : {0u, 1u})
                    for (const std::string &n : negativeOffsets(b, int(f.bw), int(f.bh), corner ? int(f.bw) - 1 : 0,
                                                                corner ? int(f.bh) - 1 : 0))
                        if (what.find(n) == std::string::npos) {
                            fresh |= taken[n]++ == 0;
                            what += (what.empty() ? "p1c" + std::to_string((b.w0 >> 13) & 15) + " " : "; ") + n;
                        }
                if (!fresh)
                    continue;
                const uint32_t bx = k % 64, by = k / 64;
                for (const uint32_t corner : {0u, 1u}) {
                    const uint32_t x = corner ? f.bw - 1 : 0, y = corner ? f.bh - 1 : 0;
                    const float *p = &v[((by * f.bh + y) * 64 * f.bw + bx * f.bw + x) * 4];
                    printAstc(out, f.f, f.bw, f.bh, b, x, y, p, kAstcHalf, false, what);
                }
            }
        }
        fprintf(out, "};\n\n");
        // PVRTC: whole levels, Morton-ordered blocks as replaceRegion takes them, every texel's RGBA8 codes
        fprintf(
            out,
            "struct PvrtcVector {\n    uint32_t format, side;\n    uint32_t blocks[32];  // modulation, colour, a "
            "block after another\n    uint32_t want[256];   // each texel's RGBA8 codes, red in the low byte\n};\n\n");
        fprintf(out, "inline constexpr PvrtcVector kPvrtcVectors[] = {\n");

        struct P {
            uint32_t f, side;
        };

        for (const P p : {P{166, 16}, P{162, 16}, P{167, 16}, P{164, 16}, P{160, 16}, P{165, 16}, P{166, 4}, P{164, 8},
                          P{166, 1}}) {
            const uint32_t bw = (p.f - 160) / 2 % 2 == 0 ? 8 : 4;
            const uint32_t blocks = std::max(1u, p.side / bw) * std::max(1u, p.side / 4);
            std::vector<uint32_t> w(blocks * 2);
            for (auto &x : w)
                x = rng();
            id<MTLTexture> t = upload(p.f, p.side, p.side, w.data(), 0);
            const auto v = readBack(t);
            fprintf(out, "    {%u, %u, {", p.f, p.side);
            for (uint32_t i = 0; i < blocks * 2; i++)
                fprintf(out, "%s0x%08x", i ? ", " : "", w[i]);
            fprintf(out, "}, {");
            for (uint32_t i = 0; i < p.side * p.side; i++) {
                uint32_t word = 0;
                for (int c = 0; c < 4; c++)
                    word |= asDecoded(v[i * 4 + c], kAstcUnorm8, (p.f & 1) != 0, c < 3) << (8 * c);
                fprintf(out, "%s0x%08x", i ? ", " : "", word);
            }
            fprintf(out, "}},\n");
        }
        fprintf(out, "};\n");
        fclose(out);
    }
    return 0;
}
