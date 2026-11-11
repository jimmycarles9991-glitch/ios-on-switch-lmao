// SPDX-License-Identifier: MIT
// raw GM20B path: block-compressed textures over their callers' memory (RawMetalNoCopy.mm), over the mock GPU
// (NVDEC_WRITETHROUGH). The device's layout of a compressed 2D texture or 2D array is NIL's image of it
// (raw_block_layout_test holds fx::blockChainLayout to NIL's answers); blocks written through the layout read back through it
// and through the no-copy texture's getBytes, and the GPU's copies of every level out of that texture, on the raw path, give
// the same blocks. The texture's deallocator runs once it is released. Heap- or buffer-backed layouts and the uncompressed
// formats keep the m2v frontend's answer. Prints the time one 1024x1024 ASTC 4x4 level takes to go into the layout.
// usage: nocopyframe <pack> <cache> <nvk dylib>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstdlib>
#include <vector>
#include "../fx/fx_image.h"
#include "../runtime/RawMetalInternal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static int failed;

static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failed += !ok;
}

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static NSString *path(id<MTLCommandBuffer> cb)
{
    const SEL sel = NSSelectorFromString(@"switchRawPath");
    return [(id)cb respondsToSelector:sel] ? ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel) : @"device";
}

@protocol RMNoCopyCalls
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes
                                     length:(NSUInteger)length
                                 descriptor:(MTLTextureDescriptor *)descriptor
                                deallocator:(void (^)(void *, NSUInteger))deallocator;
@end

@protocol RMLayoutCalls
- (NSUInteger)size;
- (NSUInteger)alignment;
- (void)copyFromLinearBytes:(const void *)bytes
               linearOffset:(NSUInteger)offset
          linearBytesPerRow:(NSUInteger)rowStride
        linearBytesPerImage:(NSUInteger)imageStride
            toTextureMemory:(void *)memory
               textureSlice:(NSUInteger)slice
               textureLevel:(NSUInteger)level
              textureRegion:(MTLRegion)region;
- (void)copyFromTextureMemory:(const void *)memory
                 textureSlice:(NSUInteger)slice
                 textureLevel:(NSUInteger)level
                textureRegion:(MTLRegion)region
                toLinearBytes:(void *)bytes
                 linearOffset:(NSUInteger)offset
            linearBytesPerRow:(NSUInteger)rowStride
          linearBytesPerImage:(NSUInteger)imageStride;
@end

struct Case {
    MTLPixelFormat format;
    unsigned blockW, blockH, blockBytes, width, height, levels, layers;
};

static std::vector<uint8_t> blocks(const Case &c, unsigned level, unsigned layer, unsigned &across, unsigned &down)
{
    across = (std::max(1u, c.width >> level) + c.blockW - 1) / c.blockW;
    down = (std::max(1u, c.height >> level) + c.blockH - 1) / c.blockH;
    std::vector<uint8_t> v(size_t(across) * down * c.blockBytes);
    for (size_t i = 0; i < v.size(); i++)
        v[i] = uint8_t(i * 131 + level * 17 + layer * 59 + (i >> 9));
    return v;
}

static MTLTextureDescriptor *descriptor(const Case &c)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = c.layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    d.pixelFormat = c.format;
    d.width = c.width;
    d.height = c.height;
    d.mipmapLevelCount = c.levels;
    d.arrayLength = c.layers;
    d.usage = MTLTextureUsageShaderRead;
    return d;
}

static void oneCase(id<MTLDevice> dev, id<MTLCommandQueue> q, const Case &c)
{
    char what[200];
    MTLTextureDescriptor *d = descriptor(c);
    id<RMLayoutCalls> layout = ((id (*)(id, SEL, id, BOOL))objc_msgSend)(
        dev, sel_registerName("newTextureLayoutWithDescriptor:isHeapOrBufferBacked:"), d, NO);
    const bool ours = layout && !strcmp(object_getClassName(layout), "RMBlockLayout");
    raw::fx::MipChain want;
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(c.format));
    const bool laid =
        f && raw::fx::blockChainLayout(c.width, c.height, c.levels, c.layers, c.blockW, c.blockH, c.blockBytes, want);
    snprintf(what, sizeof what,
             "format %lu %ux%u, %u levels, %u layers: the device's layout is NIL's (%lu bytes, %lu aligned)",
             (unsigned long)c.format, c.width, c.height, c.levels, c.layers, ours ? (unsigned long)layout.size : 0,
             ours ? (unsigned long)layout.alignment : 0);
    expect(ours && laid && layout.size == want.bytes && layout.alignment % 16384 == 0, what);
    if (!ours)
        return;
    const size_t length = (layout.size + 16383) & ~size_t(16383);
    void *memory = nullptr;
    if (posix_memalign(&memory, std::max<size_t>(layout.alignment, 16384), length))
        return;
    std::vector<std::vector<uint8_t>> source;
    double firstUs = 0;
    for (unsigned layer = 0; layer < c.layers; layer++)
        for (unsigned level = 0; level < c.levels; level++) {
            unsigned across, down;
            source.push_back(blocks(c, level, layer, across, down));
            const auto began = std::chrono::steady_clock::now();
            [layout copyFromLinearBytes:source.back().data()
                           linearOffset:0
                      linearBytesPerRow:across * c.blockBytes
                    linearBytesPerImage:0
                        toTextureMemory:memory
                           textureSlice:layer
                           textureLevel:level
                          textureRegion:MTLRegionMake2D(0, 0, std::max(1u, c.width >> level),
                                                        std::max(1u, c.height >> level))];
            if (!layer && !level)
                firstUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
        }
    if (c.width == 1024 && c.height == 1024)
        printf("     one %ux%u level into the layout: %.1f us (%zu KiB)\n", c.width, c.height, firstUs,
               source[0].size() / 1024);
    bool same = true;
    size_t at = 0;
    for (unsigned layer = 0; layer < c.layers; layer++)
        for (unsigned level = 0; level < c.levels; level++, at++) {
            unsigned across, down;
            blocks(c, level, layer, across, down);
            std::vector<uint8_t> back(source[at].size());
            [layout copyFromTextureMemory:memory
                             textureSlice:layer
                             textureLevel:level
                            textureRegion:MTLRegionMake2D(0, 0, std::max(1u, c.width >> level),
                                                          std::max(1u, c.height >> level))
                            toLinearBytes:back.data()
                             linearOffset:0
                        linearBytesPerRow:across * c.blockBytes
                      linearBytesPerImage:0];
            same = same && back == source[at];
        }
    expect(same, "every level's blocks read back through the layout");

    __block bool freed = false;
    @autoreleasepool {
        id<MTLTexture> t = [(id<RMNoCopyCalls>)dev newTextureWithBytesNoCopy:memory
                                                                      length:length
                                                                  descriptor:d
                                                                 deallocator:^(void *, NSUInteger) {
                                                                     freed = true;
                                                                 }];
        RMTexture *x = (RMTexture *)t;
        expect(t && !strcmp(object_getClassName(t), "RMNoCopyTexture") && x->_rawOk && x->_raw.blockLinear,
               "the no-copy texture is raw's, block-linear over the caller's pages");
        if (!t)
            return;
        bool cpu = true, gpu = true, raw = true;
        at = 0;
        for (unsigned layer = 0; layer < c.layers; layer++)
            for (unsigned level = 0; level < c.levels; level++, at++) {
                unsigned across, down;
                blocks(c, level, layer, across, down);
                const MTLRegion r =
                    MTLRegionMake2D(0, 0, std::max(1u, c.width >> level), std::max(1u, c.height >> level));
                std::vector<uint8_t> back(source[at].size());
                [t getBytes:back.data()
                      bytesPerRow:across * c.blockBytes
                    bytesPerImage:0
                       fromRegion:r
                      mipmapLevel:level
                            slice:layer];
                cpu = cpu && back == source[at];
                id<MTLBuffer> out = [dev newBufferWithLength:source[at].size() options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b copyFromTexture:t
                                 sourceSlice:layer
                                 sourceLevel:level
                                sourceOrigin:r.origin
                                  sourceSize:r.size
                                    toBuffer:out
                           destinationOffset:0
                      destinationBytesPerRow:across * c.blockBytes
                    destinationBytesPerImage:source[at].size()];
                [b endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (memcmp(out.contents, source[at].data(), source[at].size())) {
                    const uint8_t *o = static_cast<const uint8_t *>(out.contents);
                    size_t first = 0;
                    while (o[first] == source[at][first])
                        first++;
                    printf("     layer %u level %u (%ux%u blocks): first wrong byte %zu of %zu\n", layer, level, across,
                           down, first, source[at].size());
                    gpu = false;
                }
                raw = raw && [path(cb) isEqualToString:@"raw"];
            }
        expect(cpu, "getBytes gives every level's blocks");
        expect(gpu, "the GPU's copy of every level out of it gives its blocks");
        expect(raw, "those copies ran on the raw path");
    }
    expect(freed, "its deallocator ran once it was released");
    free(memory);
}

static void todaysAnswers(id<MTLDevice> dev, id<MTLDevice> m2v)
{
    const SEL sel = sel_registerName("newTextureLayoutWithDescriptor:isHeapOrBufferBacked:");
    auto ask = [&](id<MTLDevice> device, MTLTextureDescriptor *d, BOOL backed) {
        return ((id (*)(id, SEL, id, BOOL))objc_msgSend)(device, sel, d, backed);
    };
    MTLTextureDescriptor *astc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR
                                                                                    width:256
                                                                                   height:256
                                                                                mipmapped:YES];
    MTLTextureDescriptor *bgra = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:256
                                                                                   height:256
                                                                                mipmapped:YES];
    MTLTextureDescriptor *pvrtc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatPVRTC_RGBA_4BPP
                                                                                     width:256
                                                                                    height:256
                                                                                 mipmapped:NO];
    const std::pair<MTLTextureDescriptor *, BOOL> asks[] = {{astc, YES}, {bgra, NO}, {bgra, YES}, {pvrtc, NO}};
    bool same = true;
    for (const auto &[d, backed] : asks) {
        id<RMLayoutCalls> mine = ask(dev, d, backed), theirs = ask(m2v, d, backed);
        const bool match =
            (!mine && !theirs) || (mine && theirs && !strcmp(object_getClassName(mine), object_getClassName(theirs)) &&
                                   mine.size == theirs.size && mine.alignment == theirs.alignment);
        printf("     format %lu backed %d: %s / %s\n", (unsigned long)d.pixelFormat, backed,
               mine ? object_getClassName(mine) : "nil", theirs ? object_getClassName(theirs) : "nil");
        same = same && match;
    }
    expect(same, "heap- or buffer-backed layouts and the uncompressed formats get the m2v frontend's answer");
    __block bool freed = false;
    void *memory = nullptr;
    if (!posix_memalign(&memory, 16384, 16384 * 32)) {
        id<MTLTexture> t = [(id<RMNoCopyCalls>)dev newTextureWithBytesNoCopy:memory
                                                                      length:16384 * 32
                                                                  descriptor:bgra
                                                                 deallocator:^(void *, NSUInteger) {
                                                                     freed = true;
                                                                 }];
        expect(!t && !freed, "a no-copy texture of an uncompressed format is refused, as the frontend refuses it");
        free(memory);
    }
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: nocopyframe <pack> <cache> <nvk dylib>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVDEC_WRITETHROUGH", "1", 0);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s %s\n", nmb_last_error(), why.UTF8String);
            return 1;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const Case cases[] = {{MTLPixelFormatASTC_4x4_LDR, 4, 4, 16, 1024, 1024, 11, 1},
                              {MTLPixelFormatASTC_4x4_sRGB, 4, 4, 16, 84, 84, 7, 1},
                              {MTLPixelFormatASTC_12x12_LDR, 12, 12, 16, 100, 70, 7, 1},
                              {MTLPixelFormatASTC_8x8_LDR, 8, 8, 16, 64, 64, 7, 3},
                              {MTLPixelFormatBC1_RGBA, 4, 4, 8, 30, 20, 5, 1}};
        for (const Case &c : cases)
            oneCase(dev, q, c);
        todaysAnswers(dev, m2v);
        printf("%s: nocopyframe\n", failed ? "FAIL" : "PASS");
        return failed ? 1 : 0;
    }
}
