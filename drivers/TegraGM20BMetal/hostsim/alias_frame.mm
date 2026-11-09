// SPDX-License-Identifier: MIT
// One copy of the raw GM20B path's memory on the GPU, through the whole stack (RawMetal.mm over the frontend, m2v and NVK): m2v's
// views of the raw path's memory are made over the raw path's own BOs, and every IOSurface is imported once, by whichever driver
// needs it first. Over raw/pack's mock GPU, whose copy engine runs the raw path's copies through the mock VM (RAWCAP_LOG,
// NVDEC_WRITETHROUGH); m2v's own blits are compute dispatches the mock does not run (nvk_alias.cpp checks NVK's addressing):
//  - raw buffers and textures (pool, block-linear when private, and write-combined) and a raw surface texture, used by command
//    buffers that replay on m2v;
//  - a surface m2v imports first (newBufferWithIOSurface), which a raw texture over it reads through the same import;
//  - a surface the raw path imports first, which m2v's view and replay use.
// Every view is made with no BoImport and no VmBind of imported pages, and each surface is imported once.
// usage: alias_frame <cache dir> <host libvulkan_nouveau.dylib>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../raw/runtime/RawMetal.h"
#include "../raw/runtime/raw_pack.h"
#include "NativeBridgeMetalFrontend.h"
#include "TegraGM20BABI.h"
#include "bridge/NativeMetalObjects.h"

// Metal's SPI, which the frontend answers through its surface imports
@protocol AliasSurfaceBuffer
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)surface;
@end

extern "C" unsigned long mock_iokit_calls(unsigned method);
extern "C" unsigned long mock_iokit_import_maps(void);

namespace {
int failures;

void missing(const char *, const char *)
{}

void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    failures += !ok;
}

// A pack with no pipelines: the raw device opens; every draw would go to m2v (none here).
std::string emptyPack(const char *dir)
{
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    for (auto &s : h.section)
        s.offset = sizeof h;
    const std::string path = std::string(dir) + "/alias-empty.pack";
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(&h, sizeof h, 1, f);
    fclose(f);
    return path;
}

struct Calls {
    unsigned long imports, importMaps;

    static Calls now()
    {
        return {mock_iokit_calls(TegraGM20BMethodBoImport), mock_iokit_import_maps()};
    }
};

// A count in the frontend's or the raw path's rates line (native_bridge_metal_rates, raw_metal_rates), since its last call
uint64_t rate(size_t (*rates)(char *, size_t), const char *key)
{
    char line[2048] = "";
    rates(line, sizeof line);
    const char *at = strstr(line, key);
    return at ? strtoull(at + strlen(key), nullptr, 10) : 0;
}

// No BoImport and no VmBind of imported pages since `before`
void expectNoImport(const Calls &before, const char *what)
{
    const Calls now = Calls::now();
    char line[200];
    snprintf(line, sizeof line, "%s: m2v's view made with no BoImport (%lu) and no VmBind of imported pages (%lu)",
             what, now.imports - before.imports, now.importMaps - before.importMaps);
    expect(now.imports == before.imports && now.importMaps == before.importMaps, line);
}

void expectImports(const Calls &before, unsigned long n, const char *what)
{
    char line[200];
    snprintf(line, sizeof line, "%s: %lu BoImport for both drivers (%lu)", what, n,
             Calls::now().imports - before.imports);
    expect(Calls::now().imports - before.imports == n, line);
}

IOSurfaceRef surface(unsigned w, unsigned h)
{
    NSDictionary *p = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(w * 4),
        (id)kIOSurfacePixelFormat : @(0x42475241u),  // 'BGRA'
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

void finish(id<MTLCommandBuffer> cb)
{
    [cb commit];
    [cb waitUntilCompleted];
}

void fill(id<MTLDevice> dev, id<MTLBuffer> b, uint8_t value)
{
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e fillBuffer:b range:NSMakeRange(0, b.length) value:value];
    [e endEncoding];
    finish(cb);
}

id<MTLTexture> texture(id<MTLDevice> dev, unsigned w, unsigned h, MTLStorageMode mode)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:NO];
    d.storageMode = mode;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    return [dev newTextureWithDescriptor:d];
}

// The raw path's texture `t` replayed on m2v: a copy into a buffer behind a fill, which sends the command buffer to m2v
void replayOnM2v(id<MTLDevice> dev, id<MTLTexture> t, const char *what)
{
    id<MTLBuffer> out = [dev newBufferWithLength:t.width * t.height * 4 options:MTLResourceStorageModeShared];
    rate(raw_metal_rates, " m2v=");
    rate(native_bridge_metal_rates, " stx=");
    const Calls before = Calls::now();
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b fillBuffer:out range:NSMakeRange(0, out.length) value:0];
    [b copyFromTexture:t
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(t.width, t.height, 1)
                        toBuffer:out
               destinationOffset:0
          destinationBytesPerRow:t.width * 4
        destinationBytesPerImage:t.width * t.height * 4];
    [b endEncoding];
    finish(cb);
    char line[200];
    snprintf(line, sizeof line, "%s: the command buffer replayed on m2v and made the texture's view", what);
    expect(rate(raw_metal_rates, " m2v=") == 1 && rate(native_bridge_metal_rates, " stx=") >= 1, line);
    expectNoImport(before, what);
}

// The raw path's buffer `b` used by a command buffer that replays on m2v (a fill, which the raw path records as a call)
void rawBuffers(id<MTLDevice> dev)
{
    for (MTLResourceOptions mode : {MTLResourceStorageModePrivate, MTLResourceStorageModeShared}) {
        const char *what = mode == MTLResourceStorageModePrivate ? "pool buffer" : "write-combined buffer";
        id<MTLBuffer> b = [dev newBufferWithLength:3072 options:mode];
        rate(raw_metal_rates, " m2v=");
        const Calls before = Calls::now();
        fill(dev, b, 0x5a);
        char line[200];
        snprintf(line, sizeof line, "%s: the command buffer replayed on m2v", what);
        expect(rate(raw_metal_rates, " m2v=") == 1, line);
        expectNoImport(before, what);
    }
}

void rawTextures(id<MTLDevice> dev)
{
    for (MTLStorageMode mode : {MTLStorageModePrivate, MTLStorageModeShared}) {
        id<MTLTexture> t = texture(dev, 96, 40, mode);
        expect(raw_metal_texture_kept(t), "the raw path keeps the texture");
        // a private one is block-linear (no rows the CPU could take), its m2v view NVK's optimal image in place
        expect((raw_metal_texture_image(t, nullptr) == nullptr) == (mode == MTLStorageModePrivate),
               mode == MTLStorageModePrivate ? "pool texture: block-linear" : "write-combined texture: rows");
        replayOnM2v(dev, t, mode == MTLStorageModePrivate ? "block-linear pool texture" : "write-combined texture");
    }
}

// m2v imports a surface first; the raw path's texture over it is the same import, which a raw copy reads.
void surfaceM2vFirst(id<MTLDevice> dev, id<MTLDevice> m2v)
{
    const unsigned w = 64, h = 32, n = w * h * 4;
    IOSurfaceRef s = surface(w, h);
    const Calls first = Calls::now();
    id<MTLBuffer> view = [(id<AliasSurfaceBuffer>)m2v newBufferWithIOSurface:s];
    expect(view != nil, "m2v-first surface: m2v's buffer over it");
    std::vector<uint8_t> bytes(n);
    for (unsigned i = 0; i < n; i++)
        bytes[i] = uint8_t(i * 131u + 7u);
    memcpy(IOSurfaceGetBaseAddress(s), bytes.data(), n);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> raw = [dev newTextureWithDescriptor:d iosurface:s plane:0];
    expect(raw_metal_texture_kept(raw), "m2v-first surface: the raw path keeps its texture over it");
    id<MTLBuffer> out = [dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    memset(out.contents, 0, n);
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:raw
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:out
               destinationOffset:0
          destinationBytesPerRow:w * 4
        destinationBytesPerImage:n];
    [b endEncoding];
    finish(cb);
    expect(!memcmp(out.contents, bytes.data(), n), "m2v-first surface: a raw copy reads it through m2v's import");
    expectImports(first, 1, "m2v-first surface");
    CFRelease(s);
}

// The raw path imports a surface first; m2v's view of it, and the replay's, are that import.
void surfaceRawFirst(id<MTLDevice> dev, id<MTLDevice> m2v)
{
    const unsigned w = 48, h = 24;
    IOSurfaceRef s = surface(w, h);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    const Calls first = Calls::now();
    id<MTLTexture> raw = [dev newTextureWithDescriptor:d iosurface:s plane:0];
    expect(raw_metal_texture_kept(raw), "raw-first surface: the raw path keeps its texture over it");
    replayOnM2v(dev, raw, "raw-first surface, replayed");
    const Calls before = Calls::now();
    id<MTLBuffer> view = [(id<AliasSurfaceBuffer>)m2v newBufferWithIOSurface:s];
    expect(view != nil, "raw-first surface: m2v's buffer over it");
    expectNoImport(before, "raw-first surface");
    expectImports(first, 1, "raw-first surface");
    CFRelease(s);
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: alias_frame <cache dir> <host libvulkan_nouveau.dylib>\n");
            return 2;
        }
        setenv("NVDEC_WRITETHROUGH", "1", 1);
        setenv("RAWCAP_LOG", "/dev/null", 0);
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[2], argv[1], 0, missing);
        if (!m2v) {
            fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        NSString *why = nil;
        id<MTLDevice> dev = raw_metal_device_open(emptyPack(argv[1]).c_str(), 0, m2v, &why);
        expect(dev != nil, "the raw device opens on NVK's connection");
        if (!dev) {
            fprintf(stderr, "%s\n", why.UTF8String);
            return 1;
        }
        rawBuffers(dev);
        rawTextures(dev);
        surfaceM2vFirst(dev, m2v);
        surfaceRawFirst(dev, m2v);
    }
    printf("%s\n", failures ? "FAILED" : "ALL OK");
    return failures ? 1 : 0;
}
