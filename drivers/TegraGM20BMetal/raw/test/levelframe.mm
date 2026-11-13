// SPDX-License-Identifier: MIT
// raw GM20B path: copies into and out of a mipmapped texture's later levels (its chain, raw/fx) through the facade, over the
// mock GPU whose copy engine nvdec runs and whose 2D engine raw/fx/test/twod_sim runs (NVDEC_WRITETHROUGH): a chain generated,
// its level 0 written again (the chain's level 0 behind it), then part of level 2 from a buffer, part of level 1 from a texture;
// every level read back into buffers and textures, then Metal's whole-texture copies (into a chain of the same shape, into a
// texture of level 1's size) and a two-level copy into a smaller chain, each read back. Every level is checked byte for byte
// against the CPU's model (fx::referenceDownsample for generated levels); the facade must keep every command buffer raw.
// usage: levelframe <pack> <cache> <nvk dylib>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <string>
#include <vector>
#include "../fx/fx_mip.h"
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

using Bytes = std::vector<uint8_t>;

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static Bytes pattern(size_t n, unsigned seed)
{
    Bytes b(n);
    for (size_t i = 0; i < n; i++)
        b[i] = uint8_t(i * 131u + (i >> 7) * 17u + seed);
    return b;
}

struct Chain {
    id<MTLTexture> tex;
    std::vector<unsigned> w, h;
    std::vector<Bytes> model;
    MTLPixelFormat format = MTLPixelFormatBGRA8Unorm;
    unsigned bpp = 4;
};

static Chain makeChain(id<MTLDevice> dev, unsigned width, unsigned height, unsigned levels,
                       MTLPixelFormat format = MTLPixelFormatBGRA8Unorm, unsigned bpp = 4)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:levels > 1];
    d.mipmapLevelCount = levels;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    Chain c{[dev newTextureWithDescriptor : d], {}, {}, {}, format, bpp};
    for (unsigned l = 0; l < levels; l++) {
        c.w.push_back(std::max(1u, width >> l));
        c.h.push_back(std::max(1u, height >> l));
        c.model.emplace_back(size_t(c.w[l]) * c.h[l] * bpp);
    }
    return c;
}

static void put(Bytes &level, unsigned levelWidth, const Bytes &rect, unsigned x, unsigned y, unsigned w, unsigned h)
{
    for (unsigned r = 0; r < h; r++)
        memcpy(&level[(size_t(y + r) * levelWidth + x) * 4], &rect[size_t(r) * w * 4], size_t(w) * 4);
}

struct Run {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    int failed = 0;

    template <class Encode> bool blit(const char *what, Encode encode)
    {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        encode(b);
        [b endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) {
            printf("FAIL %s: %s\n", what, cb.error.localizedDescription.UTF8String);
            failed++;
            return false;
        }
        return true;
    }

    id<MTLBuffer> buffer(const Bytes &b)
    {
        return [dev newBufferWithBytes:b.data() length:b.size() options:MTLResourceStorageModeShared];
    }

    void check(const std::string &what, const Bytes &got, const Bytes &want)
    {
        size_t bad = 0;
        for (size_t i = 0; i < got.size(); i++)
            bad += got[i] != want[i];
        printf("%s %s: %zu bytes, %zu differ\n", bad ? "FAIL" : "OK  ", what.c_str(), got.size(), bad);
        failed += bad != 0;
    }

    // every level, out through a buffer (copyFromTexture:toBuffer:) and through a texture of its size
    void readBack(const std::string &name, const Chain &c)
    {
        const size_t levels = c.model.size();
        std::vector<id<MTLBuffer>> out(levels);
        std::vector<id<MTLTexture>> texels(levels);
        for (size_t l = 0; l < levels; l++) {
            out[l] = [dev newBufferWithLength:c.model[l].size() options:MTLResourceStorageModeShared];
            MTLTextureDescriptor *o = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.format
                                                                                         width:c.w[l]
                                                                                        height:c.h[l]
                                                                                     mipmapped:NO];
            o.storageMode = MTLStorageModeShared;
            texels[l] = [dev newTextureWithDescriptor:o];
        }
        if (!blit(name.c_str(), [&](id<MTLBlitCommandEncoder> b) {
                for (size_t l = 0; l < levels; l++) {
                    const MTLSize size = MTLSizeMake(c.w[l], c.h[l], 1);
                    [b copyFromTexture:c.tex
                                     sourceSlice:0
                                     sourceLevel:l
                                    sourceOrigin:MTLOriginMake(0, 0, 0)
                                      sourceSize:size
                                        toBuffer:out[l]
                               destinationOffset:0
                          destinationBytesPerRow:c.w[l] * c.bpp
                        destinationBytesPerImage:0];
                    [b copyFromTexture:c.tex
                              sourceSlice:0
                              sourceLevel:l
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:size
                                toTexture:texels[l]
                         destinationSlice:0
                         destinationLevel:0
                        destinationOrigin:MTLOriginMake(0, 0, 0)];
                }
            }))
            return;
        for (size_t l = 0; l < levels; l++) {
            const std::string at = name + " level " + std::to_string(l) + " (" + std::to_string(c.w[l]) + "x" +
                                   std::to_string(c.h[l]) + ")";
            Bytes got((uint8_t *)out[l].contents, (uint8_t *)out[l].contents + c.model[l].size());
            check(at + " to a buffer", got, c.model[l]);
            [texels[l] getBytes:got.data()
                    bytesPerRow:c.w[l] * c.bpp
                     fromRegion:MTLRegionMake2D(0, 0, c.w[l], c.h[l])
                    mipmapLevel:0];
            check(at + " to a texture", got, c.model[l]);
        }
    }
};

static void generated(Chain &c)
{
    for (size_t l = 1; l < c.model.size(); l++)
        raw::fx::referenceDownsample(c.model[l - 1].data(), c.w[l - 1] * 4, c.w[l - 1], c.h[l - 1], c.model[l].data(),
                                     c.w[l] * 4, c.w[l], c.h[l], 4);
}

static void upload(Run &run, Chain &c, unsigned seed)
{
    c.model[0] = pattern(c.model[0].size(), seed);
    id<MTLBuffer> src = run.buffer(c.model[0]);
    run.blit("level 0 in", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromBuffer:src
                   sourceOffset:0
              sourceBytesPerRow:c.w[0] * 4
            sourceBytesPerImage:0
                     sourceSize:MTLSizeMake(c.w[0], c.h[0], 1)
                      toTexture:c.tex
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    });
}

static void levels(Run &run, unsigned width, unsigned height, unsigned count)
{
    const std::string name = std::to_string(width) + "x" + std::to_string(height);
    Chain t = makeChain(run.dev, width, height, count);
    upload(run, t, 5);
    generated(t);
    run.blit("generate", [&](id<MTLBlitCommandEncoder> b) {
        [b generateMipmapsForTexture:t.tex];
    });
    // level 0 again: the chain's level 0 is behind it, its later levels still the generated ones
    upload(run, t, 77);

    const unsigned w2 = t.w[2] - 1, h2 = t.h[2] - 1;
    const Bytes into2 = pattern(size_t(w2) * h2 * 4, 9);
    id<MTLBuffer> rows = run.buffer(into2);
    run.blit("buffer into level 2", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromBuffer:rows
                   sourceOffset:0
              sourceBytesPerRow:w2 * 4
            sourceBytesPerImage:0
                     sourceSize:MTLSizeMake(w2, h2, 1)
                      toTexture:t.tex
               destinationSlice:0
               destinationLevel:2
              destinationOrigin:MTLOriginMake(1, 1, 0)];
    });
    put(t.model[2], t.w[2], into2, 1, 1, w2, h2);

    const unsigned w1 = t.w[1] - 3, h1 = t.h[1] - 2;
    MTLTextureDescriptor *pd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w1 + 2
                                                                                 height:h1 + 1
                                                                              mipmapped:NO];
    pd.storageMode = MTLStorageModeShared;
    id<MTLTexture> piece = [run.dev newTextureWithDescriptor:pd];
    const Bytes whole = pattern(size_t(w1 + 2) * (h1 + 1) * 4, 41);
    [piece replaceRegion:MTLRegionMake2D(0, 0, w1 + 2, h1 + 1)
             mipmapLevel:0
               withBytes:whole.data()
             bytesPerRow:(w1 + 2) * 4];
    run.blit("texture into level 1", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromTexture:piece
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(2, 1, 0)
                   sourceSize:MTLSizeMake(w1, h1, 1)
                    toTexture:t.tex
             destinationSlice:0
             destinationLevel:1
            destinationOrigin:MTLOriginMake(3, 2, 0)];
    });
    Bytes rect(size_t(w1) * h1 * 4);
    for (unsigned r = 0; r < h1; r++)
        memcpy(&rect[size_t(r) * w1 * 4], &whole[(size_t(r + 1) * (w1 + 2) + 2) * 4], size_t(w1) * 4);
    put(t.model[1], t.w[1], rect, 3, 2, w1, h1);
    run.readBack(name, t);

    Chain same = makeChain(run.dev, width, height, count);
    run.blit("whole copy", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromTexture:t.tex toTexture:same.tex];
    });
    same.model = t.model;
    run.readBack(name + " whole copy", same);

    Chain half = makeChain(run.dev, t.w[1], t.h[1], 1);
    run.blit("whole copy into level 1's size", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromTexture:t.tex toTexture:half.tex];
    });
    half.model[0] = t.model[1];
    run.readBack(name + " into level 1's size", half);

    Chain two = makeChain(run.dev, t.w[1], t.h[1], 2);
    run.blit("levels 1 and 2", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromTexture:t.tex
                 sourceSlice:0
                 sourceLevel:1
                   toTexture:two.tex
            destinationSlice:0
            destinationLevel:0
                  sliceCount:1
                  levelCount:2];
    });
    two.model = {t.model[1], t.model[2]};
    run.readBack(name + " levels 1 and 2", two);

    // a chain never generated: the part of level 2 written stays through the read's making the chain current
    Chain fresh = makeChain(run.dev, width, height, count);
    upload(run, fresh, 3);
    id<MTLBuffer> part = [run.dev newBufferWithLength:into2.size() options:MTLResourceStorageModeShared];
    run.blit("part of level 2 of a fresh chain", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromBuffer:rows
                   sourceOffset:0
              sourceBytesPerRow:w2 * 4
            sourceBytesPerImage:0
                     sourceSize:MTLSizeMake(w2, h2, 1)
                      toTexture:fresh.tex
               destinationSlice:0
               destinationLevel:2
              destinationOrigin:MTLOriginMake(1, 1, 0)];
    });
    run.blit("part of level 2 out", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromTexture:fresh.tex
                         sourceSlice:0
                         sourceLevel:2
                        sourceOrigin:MTLOriginMake(1, 1, 0)
                          sourceSize:MTLSizeMake(w2, h2, 1)
                            toBuffer:part
                   destinationOffset:0
              destinationBytesPerRow:w2 * 4
            destinationBytesPerImage:0];
    });
    run.check(name + " part of level 2 of a fresh chain",
              Bytes((uint8_t *)part.contents, (uint8_t *)part.contents + into2.size()), into2);
}

// A chain of a format the 2D engine does not filter (raw::fx::twodFormat 0): every level written from a buffer and read back
// (into a buffer and into a texture of the level's size: the copy engine's, as are the writes), then its generateMipmaps, which
// m2v makes in the chain's own memory under its own predicate (mipmaps.fmt)
static void formatLevels(Run &run, MTLPixelFormat format, unsigned bpp, const char *name)
{
    Chain t = makeChain(run.dev, 96, 40, 5, format, bpp);
    if (!raw_metal_texture_kept(t.tex)) {
        printf("FAIL %s: not a raw chain\n", name);
        run.failed++;
        return;
    }
    for (unsigned l = 0; l < t.model.size(); l++) {
        t.model[l] = pattern(t.model[l].size(), 11 + l);
        id<MTLBuffer> src = run.buffer(t.model[l]);
        run.blit("a level in", [&](id<MTLBlitCommandEncoder> b) {
            [b copyFromBuffer:src
                       sourceOffset:0
                  sourceBytesPerRow:t.w[l] * bpp
                sourceBytesPerImage:0
                         sourceSize:MTLSizeMake(t.w[l], t.h[l], 1)
                          toTexture:t.tex
                   destinationSlice:0
                   destinationLevel:l
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        });
    }
    run.readBack(name, t);
    char rates[1024];
    raw_metal_rates(rates, sizeof rates);
    run.blit("generate", [&](id<MTLBlitCommandEncoder> b) {
        [b generateMipmapsForTexture:t.tex];
    });
    const bool counted = raw_metal_rates(rates, sizeof rates) && strstr(rates, "mipmaps.fmt");
    printf("%s %s generateMipmaps: m2v's, counted as mipmaps.fmt\n", counted ? "OK  " : "FAIL", name);
    run.failed += !counted;
}

// A later level into rows 192 bytes apart (a texture over a buffer): the copy engine's copy, so the command buffer stays raw
static void unalignedRows(Run &run)
{
    Chain t = makeChain(run.dev, 96, 40, 5);
    upload(run, t, 21);
    generated(t);
    run.blit("generate", [&](id<MTLBlitCommandEncoder> b) {
        [b generateMipmapsForTexture:t.tex];
    });
    const unsigned w = t.w[1], h = t.h[1], row = w * 4;
    id<MTLBuffer> backing = [run.dev newBufferWithLength:size_t(row) * h options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> rows = [backing newTextureWithDescriptor:d offset:0 bytesPerRow:row];
    char rates[1024];
    raw_metal_rates(rates, sizeof rates);
    run.blit("level 1 into rows", [&](id<MTLBlitCommandEncoder> b) {
        [b copyFromTexture:t.tex
                  sourceSlice:0
                  sourceLevel:1
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:rows
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    });
    const bool raw = raw_metal_rates(rates, sizeof rates) && strstr(rates, " m2v=0 ");
    run.check("96x40 level 1 into rows 192 bytes apart",
              Bytes((uint8_t *)backing.contents, (uint8_t *)backing.contents + size_t(row) * h), t.model[1]);
    printf("%s 96x40 level 1 into rows 192 bytes apart: %s\n", raw ? "OK  " : "FAIL", raw ? "raw" : "m2v");
    run.failed += !raw;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: levelframe <pack> <cache> <nvk dylib>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        if (!m2v) {
            fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        NSString *why = nil;
        id<MTLDevice> dev = raw_metal_device_open(argv[1], 0, m2v, &why);
        if (!dev) {
            fprintf(stderr, "NO RAW DEVICE: %s\n", why.UTF8String);
            return 1;
        }
        Run run{dev, [dev newCommandQueue]};
        levels(run, 96, 40, 5);
        levels(run, 33, 17, 4);
        unalignedRows(run);
        formatLevels(run, MTLPixelFormat(43), 2, "96x40 BGR5A1Unorm");
        formatLevels(run, MTLPixelFormatRGB10A2Unorm, 4, "96x40 RGB10A2Unorm");
        char rates[1024];
        if (raw_metal_rates(rates, sizeof rates))
            printf("RATES %s\n", rates);
        printf("levelframe: %d case(s) failed\n", run.failed);
        return run.failed ? 1 : 0;
    }
}
