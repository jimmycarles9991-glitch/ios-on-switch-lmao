// SPDX-License-Identifier: MIT
// raw GM20B path (r7-raw, r8-rawca): test/frame.h's frames through the shipped Metal frontend + m2v + NVK host twin, over the mock GPU (capture/cap.cpp decodes
// every submission when RAWCAP_LOG is set). Also the "current path" encode benchmark: CPU time per frame on the encoding thread.
// usage: nvkframe <nvk dylib> <cache dir> <default.metallib> <corpus.log> <frames> [blurPasses draws uniformBytes]
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <cmath>
#import <objc/message.h>
#include <algorithm>
#include <chrono>
#include <mach/mach.h>
#include "../test/frame.h"
#include "state_desc.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#ifdef RAW_FACADE  // the same frames through the raw path's Metal facade (runtime/RawMetal.mm) instead of the frontend + m2v + NVK
#include <IOKit/IOReturn.h>
#include "../runtime/RawMetal.h"
#include "TegraGM20BABI.h"
#define FRAME_PATH "raw-facade"
extern "C" void mock_iokit_fail_next(unsigned method, unsigned count, int kr);
#else
#define FRAME_PATH "nvk"
#endif

extern "C" void pushcount_report(const char *what);

// What Core Animation does with every command buffer it makes: no content protection (setProtectionOptions:, SPI)
static void caCommandBuffer(id<MTLCommandBuffer> cb)
{
    static const SEL protection = NSSelectorFromString(@"setProtectionOptions:");
    if ([(id)cb respondsToSelector:protection])
        ((void (*)(id, SEL, uint64_t))objc_msgSend)(cb, protection, 0);
}

// FRAME_STATES_OUT=<file>: the frame's states' corpus lines appended to it (the corpus of a pack of just them)
static void statesOut(const frame::State &s)
{
    if (const char *out = getenv("FRAME_STATES_OUT"))
        if (FILE *f = fopen(out, "a")) {
            fprintf(f, "%s\n", s.line.c_str());
            fclose(f);
        }
}

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static uint64_t threadNs()
{
    thread_basic_info_data_t t{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&t, &n);
    mach_port_deallocate(mach_task_self(), self);
    return ((uint64_t)t.user_time.seconds * 1000000 + t.user_time.microseconds +
            (uint64_t)t.system_time.seconds * 1000000 + t.system_time.microseconds) *
           1000;
}

// FRAME_RATES=1: the rates lines after every frame (that frame's counters): RATESF (raw facade) and M2VF (m2v). Called once
// before the first frame with -1, for m2v's baseline (its first call only takes one).
static void framesRates(int f)
{
    if (!getenv("FRAME_RATES"))
        return;
    char rates[4096];
#ifdef RAW_FACADE
    if (f >= 0 && raw_metal_rates(rates, sizeof rates))
        printf("RATESF %d %s\n", f, rates);
#endif
    if (native_bridge_metal_rates(rates, sizeof rates) && f >= 0)
        printf("M2VF %d %s\n", f, rates);
}

using state_desc::descriptor;

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 6) {
            fprintf(stderr,
                    "usage: nvkframe <nvk> <cache> <metallib> <corpus> <frames> [blurPasses draws uniformBytes]\n");
            return 2;
        }
        const unsigned frames = atoi(argv[5]);
        frame::Shape shape;
        if (argc > 6)
            shape.blurPasses = atoi(argv[6]);
        if (argc > 7)
            shape.draws = atoi(argv[7]);
        if (argc > 8)
            shape.uniformBytes = atoi(argv[8]);
        if (argc > 9)
            shape.vertexBytes = atoi(argv[9]);
#ifdef RAW_FACADE
        // argv[1]: the pack. FRAME_FALLBACK=<nvk dylib>: the m2v device under it (the render server's arrangement), else none
        id<MTLDevice> fallback = nil;
        if (const char *nvk = getenv("FRAME_FALLBACK")) {
            setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
            fallback = native_bridge_metal_open(nvk, argv[2], 0, missing);
            if (!fallback) {
                fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
                return 1;
            }
        }
        NSString *why = nil;
        id<MTLDevice> dev = raw_metal_device_open(argv[1], 0, fallback, &why);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", why.UTF8String);
            return 1;
        }
        // FRAME_REFUSE_CHANNEL: the kernel refuses the raw device's first channel (its frames go to m2v until it gets one)
        if (getenv("FRAME_REFUSE_CHANNEL"))
            mock_iokit_fail_next(TegraGM20BMethodChannelCreate, 1, kIOReturnNoResources);
        // FRAME_COMPLETE=<dir>|memory: the runtime makes the records of the states the pack lacks (RawComplete.h), kept in <dir>;
        // FRAME_COMPLETE_SETTLE=<frame>: two seconds before that frame, so the frames after draw the states it made
        if (const char *complete = getenv("FRAME_COMPLETE"))
            raw_metal_complete_states(strcmp(complete, "memory") ? complete : nullptr);
#else
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
#endif
        NSError *error = nil;
        // FRAME_LIBRARY_BYTES=1: the library made from its bytes, as an app's own (no framework's: no state looked up in the
        // pack, every one the runtime's to make)
        id<MTLLibrary> qc = nil;
        if (getenv("FRAME_LIBRARY_BYTES")) {
            NSData *bytes = [NSData dataWithContentsOfFile:@(argv[3])];
            qc = [dev newLibraryWithData:dispatch_data_create(bytes.bytes, bytes.length, nil,
                                                              DISPATCH_DATA_DESTRUCTOR_DEFAULT)
                                   error:&error];
        } else {
            qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        }
        std::vector<frame::State> all = frame::states(argv[4]);
        std::vector<id<MTLRenderPipelineState>> uber, blur, extra;
        double createUs = 0;  // the frame's states' creation (newRenderPipelineStateWithDescriptor), on this thread
        for (auto &s : all) {
            if (s.extra) {
                if (id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:descriptor(qc, s.line)
                                                                                       error:&error]) {
                    extra.push_back(p);
                    printf("STATE extra %s\n", s.key.c_str());
                    statesOut(s);
                }
                continue;
            }
            if (!s.uber && !s.blur)
                continue;
            if (s.uber && uber.size() >= shape.mainStates)
                continue;
            if (s.blur && blur.size() >= 4 && !getenv("FRAME_ALL_STATES"))
                continue;
            MTLRenderPipelineDescriptor *made = descriptor(qc, s.line);
            const auto creating = std::chrono::steady_clock::now();
            id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:made error:&error];
            createUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - creating).count();
            if (!p) {
                fprintf(stderr, "REFUSED %s\n", s.key.c_str());
#ifdef RAW_FACADE
                fprintf(stderr, "FACADE  %s\n", raw_metal_pipeline_key(descriptor(qc, s.line)).UTF8String);
#endif
                return 1;
            }
            (s.uber ? uber : blur).push_back(p);
            printf("STATE %s %s\n", s.uber ? "uber" : "blur", s.key.c_str());
            statesOut(s);
        }
        printf("CREATE %zu states, %.0f us a state\n", uber.size() + blur.size(),
               uber.size() + blur.size() ? createUs / double(uber.size() + blur.size()) : 0.0);
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLTexture> tex[frame::TEX_COUNT];
        IOSurfaceRef surfaces[frame::TEX_COUNT] = {};
        for (int t = 0; t < frame::TEX_COUNT; t++) {
            frame::TexDesc td = frame::texture(t);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(td.format)
                                                                                         width:td.w
                                                                                        height:td.h
                                                                                     mipmapped:NO];
            d.usage = td.renderTarget ? MTLTextureUsageRenderTarget | (td.memoryless ? 0 : MTLTextureUsageShaderRead)
                                      : MTLTextureUsageShaderRead;
            if (t == frame::IMAGE2 && getenv("FRAME_YCBCR")) {
                // FRAME_YCBCR: IMAGE2 a decoded video frame: YCBCR8_420_2P (500) over a '420v' surface of the texture's size
                // (=padded: its planes past the texture, a decoder's padded surface whose top-left is sampled; =pitch16: rows
                // and the chroma plane's offset 16 bytes apart but not 32, as ImageIO's photo thumbnails lie, which the raw path
                // samples through pool images the copy engine fills: raw::Texture::sampled). FRAME_YCBCR_FULL: a '420f' surface
                // (full range: JPEG's, ImageIO's photos); FRAME_YCBCR_SIZE=WxH: the texture's size (a screenshot's thumbnail)
                const bool padded = !strcmp(getenv("FRAME_YCBCR"), "padded"),
                           pitch16 = !strcmp(getenv("FRAME_YCBCR"), "pitch16"), full = getenv("FRAME_YCBCR_FULL");
                frame::TexDesc yt = td;
                if (const char *size = getenv("FRAME_YCBCR_SIZE"))
                    sscanf(size, "%dx%d", &yt.w, &yt.h);
                auto odd16 = [](size_t n) {
                    n = (n + 15) & ~size_t(15);
                    return n & 31 ? n : n + 16;
                };
                const size_t w = yt.w + (padded ? 1 : 0), h = yt.h + (padded ? 3 : 0),
                             row0 = pitch16 ? odd16(w) : (w + 63) & ~size_t(63),
                             row1 = pitch16 ? odd16(((w + 1) / 2) * 2) : (((w + 1) / 2) * 2 + 63) & ~size_t(63),
                             off1 = pitch16 ? odd16(row0 * h) : (row0 * h + 4095) & ~size_t(4095);
                NSDictionary *p0 = @{
                    (id)kIOSurfacePlaneWidth : @(w),
                    (id)kIOSurfacePlaneHeight : @(h),
                    (id)kIOSurfacePlaneBytesPerElement : @1,
                    (id)kIOSurfacePlaneBytesPerRow : @(row0),
                    (id)kIOSurfacePlaneOffset : @0,
                    (id)kIOSurfacePlaneSize : @(row0 * h)
                };
                NSDictionary *p1 = @{
                    (id)kIOSurfacePlaneWidth : @((w + 1) / 2),
                    (id)kIOSurfacePlaneHeight : @((h + 1) / 2),
                    (id)kIOSurfacePlaneBytesPerElement : @2,
                    (id)kIOSurfacePlaneBytesPerRow : @(row1),
                    (id)kIOSurfacePlaneOffset : @(off1),
                    (id)kIOSurfacePlaneSize : @(row1 * ((h + 1) / 2))
                };
                surfaces[t] = IOSurfaceCreate((__bridge CFDictionaryRef) @{
                    (id)kIOSurfaceWidth : @(w),
                    (id)kIOSurfaceHeight : @(h),
                    (id)kIOSurfacePixelFormat : @(full ? (uint32_t)'420f' : (uint32_t)'420v'),
                    (id)kIOSurfaceAllocSize : @(off1 + row1 * ((h + 1) / 2)),
                    (id)kIOSurfacePlaneInfo : @[ p0, p1 ]
                });
                uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(surfaces[t]);
                for (size_t i = 0; i < off1 + row1 * ((h + 1) / 2); i++)
                    base[i] = uint8_t(i * 37u + (i >> 8));
                {  // the planes' texels as the conversion must sample them (nvdec NVDEC_TEXELS hashes each pitch header's)
                    auto rows = [&](size_t at, size_t row, size_t bytes, size_t count) {
                        uint64_t hash = 0xcbf29ce484222325ull;
                        for (size_t y = 0; y < count; y++)
                            for (size_t x = 0; x < bytes; x++)
                                hash = (hash ^ base[at + y * row + x]) * 0x100000001b3ull;
                        return (unsigned long long)hash;
                    };
                    printf("YCBCR texels luma=%016llx chroma=%016llx\n", rows(0, row0, yt.w, yt.h),
                           rows(off1, row1, ((yt.w + 1) / 2) * 2, (yt.h + 1) / 2));
                    // the conversion's fragment buffer's first words (width, height, full range, red's kb: no matrix, BT.601)
                    printf("YCBCR shape=[%08x %08x %08x %08x\n", yt.w, yt.h, full ? 1u : 0u,
                           full ? uint32_t(-8) : uint32_t(-9));
                }
                MTLTextureDescriptor *yd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(500)
                                                                                              width:yt.w
                                                                                             height:yt.h
                                                                                          mipmapped:NO];
                yd.usage = MTLTextureUsageShaderRead;
                yd.storageMode = MTLStorageModeShared;
                tex[t] = [dev newTextureWithDescriptor:yd iosurface:surfaces[t] plane:0];
            } else if (t == frame::IMAGE3 && getenv("FRAME_MASK")) {
                // FRAME_MASK: IMAGE3 a rounded-rect coverage mask as Core Animation makes its corner images: A8Unorm, shared
                // storage (the raw path's write-combined memory, r17), its texels written by replaceRegion from rows of 61
                // bytes (no padding), with its expected texels (nvdec NVDEC_TEXELS)
                constexpr unsigned mw = 61, mh = 47, radius = 12;
                MTLTextureDescriptor *md =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatA8Unorm
                                                                       width:mw
                                                                      height:mh
                                                                   mipmapped:NO];
                md.usage = MTLTextureUsageShaderRead;
                md.storageMode = MTLStorageModeShared;
                tex[t] = [dev newTextureWithDescriptor:md];
                std::vector<uint8_t> m(mw * mh);
                uint64_t hash = 0xcbf29ce484222325ull;
                for (unsigned y = 0; y < mh; y++)
                    for (unsigned x = 0; x < mw; x++) {
                        const double cx = x + 0.5 < radius        ? radius
                                          : x + 0.5 > mw - radius ? mw - radius
                                                                  : x + 0.5,
                                     cy = y + 0.5 < radius        ? radius
                                          : y + 0.5 > mh - radius ? mh - radius
                                                                  : y + 0.5;
                        const double dist = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                        m[y * mw + x] = uint8_t(std::clamp(radius + 0.5 - dist, 0.0, 1.0) * 255.0 + 0.5);
                        hash = (hash ^ m[y * mw + x]) * 0x100000001b3ull;
                    }
                [tex[t] replaceRegion:MTLRegionMake2D(0, 0, mw, mh) mipmapLevel:0 withBytes:m.data() bytesPerRow:mw];
                printf("MASK texels=%016llx corner=%u centre=%u\n", (unsigned long long)hash, m[0],
                       m[(mh / 2) * mw + mw / 2]);
            } else if (t == frame::IMAGE3 && getenv("FRAME_1D")) {
                // FRAME_1D=<MTLPixelFormat>: IMAGE3 a 1D lookup table as backboardd's on iOS 27 (4096x1 R32Float/R16Float,
                // 3072x1 R16Uint, 1024x1 RG32Float): shared, its texels written by replaceRegion
                const MTLPixelFormat lf = MTLPixelFormat(atoi(getenv("FRAME_1D")) ?: MTLPixelFormatR32Float);
                MTLTextureDescriptor *ld = [MTLTextureDescriptor new];
                ld.textureType = MTLTextureType1D;
                ld.pixelFormat = lf;
                ld.width = 1024;
                ld.usage = MTLTextureUsageShaderRead;
                ld.storageMode = MTLStorageModeShared;
                tex[t] = [dev newTextureWithDescriptor:ld];
                std::vector<uint8_t> row(1024 * 16);
                for (size_t i = 0; i < row.size(); i++)
                    row[i] = uint8_t(i * 7 + 3);
                [tex[t] replaceRegion:MTLRegionMake1D(0, 1024)
                          mipmapLevel:0
                            withBytes:row.data()
                          bytesPerRow:row.size()];
            } else if (t == frame::IMAGE3 && (getenv("FRAME_3D") || getenv("FRAME_ARRAY") || getenv("FRAME_CUBE"))) {
                // FRAME_3D=<MTLPixelFormat>: IMAGE3 a 17x17x17 colour cube, as Core Animation's colour matching samples one;
                // FRAME_ARRAY=<layers>: a 17x17 RGBA8 2D array of that many layers; FRAME_CUBE=<cubes>: a 16x16 RGBA8 cube
                // (or cube array). FRAME_3D_BLIT: private, its planes or layers copied in from a buffer by a blit, else shared
                // and written by replaceRegion
                const bool volume = getenv("FRAME_3D") != nullptr, cubes = getenv("FRAME_CUBE") != nullptr;
                const MTLPixelFormat cf = MTLPixelFormat(volume ? atoi(getenv("FRAME_3D")) ?: MTLPixelFormatRGBA8Unorm
                                                                : MTLPixelFormatRGBA8Unorm);
                const NSUInteger side = cubes ? 16 : 17, bpp = cf == MTLPixelFormatRGBA16Float ? 8 : 4,
                                 row = side * bpp,
                                 count = cubes    ? NSUInteger(std::max(atoi(getenv("FRAME_CUBE")), 1))
                                         : volume ? side
                                                  : NSUInteger(std::max(atoi(getenv("FRAME_ARRAY")), 1)),
                                 images = cubes ? count * 6 : count;
                MTLTextureDescriptor *cd = [MTLTextureDescriptor new];
                cd.textureType = volume  ? MTLTextureType3D
                                 : cubes ? (count > 1 ? MTLTextureTypeCubeArray : MTLTextureTypeCube)
                                         : MTLTextureType2DArray;
                cd.pixelFormat = cf;
                cd.width = cd.height = side;
                cd.depth = volume ? side : 1;
                cd.arrayLength = volume ? 1 : count;
                cd.usage = MTLTextureUsageShaderRead;
                const bool blit = getenv("FRAME_3D_BLIT") != nullptr;
                cd.storageMode = blit ? MTLStorageModePrivate : MTLStorageModeShared;
                tex[t] = [dev newTextureWithDescriptor:cd];
                std::vector<uint8_t> cube(row * side * images);
                for (size_t i = 0; i < cube.size(); i++)
                    cube[i] = uint8_t(i * 13 + 5);
                // (a 3D texture's box at once; an array's or cube's layers one slice at a time, as Metal moves them)
                const NSUInteger moves = volume ? 1 : images, depth = volume ? side : 1, image = row * side;
                if (blit) {
                    id<MTLBuffer> from = [dev newBufferWithBytes:cube.data()
                                                          length:cube.size()
                                                         options:MTLResourceStorageModeShared];
                    id<MTLCommandBuffer> up = [queue commandBuffer];
                    id<MTLBlitCommandEncoder> be = [up blitCommandEncoder];
                    for (NSUInteger k = 0; k < moves; k++)
                        [be copyFromBuffer:from
                                   sourceOffset:k * image
                              sourceBytesPerRow:row
                            sourceBytesPerImage:image
                                     sourceSize:MTLSizeMake(side, side, depth)
                                      toTexture:tex[t]
                               destinationSlice:volume ? 0 : k
                               destinationLevel:0
                              destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [be endEncoding];
                    [up commit];
                    [up waitUntilCompleted];
                } else {
                    for (NSUInteger k = 0; k < moves; k++)
                        [tex[t] replaceRegion:MTLRegionMake3D(0, 0, 0, side, side, depth)
                                  mipmapLevel:0
                                        slice:volume ? 0 : k
                                    withBytes:cube.data() + k * image
                                  bytesPerRow:row
                                bytesPerImage:image];
                }
                // FRAME_3D_CHECK (with NVDEC_WRITETHROUGH, so the copy engine's writes reach the texture's memory): its
                // texels read back as written, on the CPU and (FRAME_3D_BLIT) through the copy engine
                if (getenv("FRAME_3D_CHECK")) {
                    std::vector<uint8_t> back(cube.size());
                    if (blit) {
                        id<MTLBuffer> out = [dev newBufferWithLength:cube.size() options:MTLResourceStorageModeShared];
                        id<MTLCommandBuffer> down = [queue commandBuffer];
                        id<MTLBlitCommandEncoder> be = [down blitCommandEncoder];
                        for (NSUInteger k = 0; k < moves; k++)
                            [be copyFromTexture:tex[t]
                                             sourceSlice:volume ? 0 : k
                                             sourceLevel:0
                                            sourceOrigin:MTLOriginMake(0, 0, 0)
                                              sourceSize:MTLSizeMake(side, side, depth)
                                                toBuffer:out
                                       destinationOffset:k * image
                                  destinationBytesPerRow:row
                                destinationBytesPerImage:image];
                        [be endEncoding];
                        [down commit];
                        [down waitUntilCompleted];
                        printf("CUBE %s\n", !memcmp(out.contents, cube.data(), cube.size()) ? "match" : "differs");
                    }
#ifdef RAW_FACADE
                    // the raw path's texture, read by the CPU in NIL's layout (private storage too)
                    for (NSUInteger k = 0; k < moves; k++)
                        [tex[t] getBytes:back.data() + k * image
                              bytesPerRow:row
                            bytesPerImage:image
                               fromRegion:MTLRegionMake3D(0, 0, 0, side, side, depth)
                              mipmapLevel:0
                                    slice:volume ? 0 : k];
                    printf("CUBE layout %s\n", back == cube ? "match" : "differs");
#endif
                }
            } else if (t == frame::BLUR0 && getenv("FRAME_FBVIEW")) {
                // FRAME_FBVIEW: BLUR0, which blur passes render into and sample, a 2D view of part of a private texture:
                // slice (layer 1 of a 3-layer array), level (level 1 of a chain twice its size), level0 (level 0 of a chain)
                const char *part = getenv("FRAME_FBVIEW");
                const bool slice = !strcmp(part, "slice"), level = !strcmp(part, "level");
                MTLTextureDescriptor *pd = [MTLTextureDescriptor new];
                pd.textureType = slice ? MTLTextureType2DArray : MTLTextureType2D;
                pd.pixelFormat = MTLPixelFormat(td.format);
                pd.width = level ? td.w * 2 : td.w;
                pd.height = level ? td.h * 2 : td.h;
                pd.arrayLength = slice ? 3 : 1;
                pd.mipmapLevelCount = slice ? 1 : 3;
                pd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                pd.storageMode = MTLStorageModePrivate;
                id<MTLTexture> parent = [dev newTextureWithDescriptor:pd];
                tex[t] = [parent newTextureViewWithPixelFormat:pd.pixelFormat
                                                   textureType:MTLTextureType2D
                                                        levels:NSMakeRange(level ? 1 : 0, 1)
                                                        slices:NSMakeRange(slice ? 1 : 0, 1)];
                printf("FBVIEW %s %lux%lu\n", part, (unsigned long)tex[t].width, (unsigned long)tex[t].height);
            } else if (t == frame::IMAGE1 &&
                       frame::mipImage()) {  // every level, private: m2v's own image, or the raw path's chain
                d.mipmapLevelCount = 1 + unsigned(std::log2(double(std::max(td.w, td.h))));
                d.storageMode = MTLStorageModePrivate;
                tex[t] = [dev newTextureWithDescriptor:d];
            } else if (td.iosurface && td.format == 542) {
                // a widget snapshot: luma rows one byte a texel, chroma rows two, each rounded up to 64 bytes (FRAME_PITCH:
                // 16, so neither is a pitch target's 128)
                const NSUInteger round = frame::pitchShapes("fb") ? 16 : 64, row0 = (td.w + round - 1) / round * round,
                                 row1 = (2 * td.w + round - 1) / round * round, off1 = (row0 * td.h + 4095) & ~4095ul;
                NSDictionary *p0 = @{
                    (id)kIOSurfacePlaneWidth : @(td.w),
                    (id)kIOSurfacePlaneHeight : @(td.h),
                    (id)kIOSurfacePlaneBytesPerElement : @1,
                    (id)kIOSurfacePlaneBytesPerRow : @(row0),
                    (id)kIOSurfacePlaneOffset : @0,
                    (id)kIOSurfacePlaneSize : @(row0 * td.h)
                };
                NSDictionary *p1 = @{
                    (id)kIOSurfacePlaneWidth : @(td.w),
                    (id)kIOSurfacePlaneHeight : @(td.h),
                    (id)kIOSurfacePlaneBytesPerElement : @2,
                    (id)kIOSurfacePlaneBytesPerRow : @(row1),
                    (id)kIOSurfacePlaneOffset : @(off1),
                    (id)kIOSurfacePlaneSize : @(row1 * td.h)
                };
                surfaces[t] = IOSurfaceCreate((__bridge CFDictionaryRef) @{
                    (id)kIOSurfaceWidth : @(td.w),
                    (id)kIOSurfaceHeight : @(td.h),
                    (id)kIOSurfacePixelFormat : @((uint32_t)'_RB4'),
                    (id)kIOSurfaceAllocSize : @((off1 + row1 * td.h + 16383) & ~16383ul),
                    (id)kIOSurfacePlaneInfo : @[ p0, p1 ]
                });
                d.storageMode = MTLStorageModeShared;
                tex[t] = [dev newTextureWithDescriptor:d iosurface:surfaces[t] plane:0];
            } else if (td.iosurface) {
                NSDictionary *p = @{
                    (id)kIOSurfaceWidth : @(td.w),
                    (id)kIOSurfaceHeight : @(td.h),
                    (id)kIOSurfaceBytesPerElement : @4,
                    (id)kIOSurfaceBytesPerRow : @(td.w * 4),
                    (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA')
                };
                surfaces[t] = IOSurfaceCreate((__bridge CFDictionaryRef)p);
                d.storageMode = MTLStorageModeShared;
                tex[t] = [dev newTextureWithDescriptor:d iosurface:surfaces[t] plane:0];
            } else if (!td.memoryless && td.format == 80 && getenv("FRAME_INPLACE")) {
                // m2v's in-place texture over page-aligned memory: what the raw path's pool textures are to m2v (their twins)
                const NSUInteger row = (td.w * 4 + 255) & ~NSUInteger(255), page = (NSUInteger)getpagesize();
                const NSUInteger bytes = (row * td.h + page - 1) / page * page;
                void *mem = nullptr;
                posix_memalign(&mem, page, bytes);
                memset(mem, 0, bytes);
                id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:mem
                                                         length:bytes
                                                        options:MTLResourceStorageModeShared
                                                    deallocator:nil];
                d.storageMode = MTLStorageModeShared;
                tex[t] = native_bridge_metal_texture_in_place(b, d, 0, row);
                if (!tex[t]) {
                    fprintf(stderr, "in-place texture %d refused\n", t);
                    return 1;
                }
            } else {
                d.storageMode = td.memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
                tex[t] = [dev newTextureWithDescriptor:d];
            }
        }
        // FRAME_VIEW: IMAGE3 a view of part of a texture. level (with FRAME_MIP): levels 1.. of IMAGE1's chain; slice (with
        // FRAME_ARRAY): a 2D view of layer 1; face (with FRAME_CUBE): a 2D view of face 2; array (with FRAME_CUBE): a 2D-array
        // view of the faces. With FRAME_3D_CHECK, a layer view reads back its layer's texels.
        if (const char *part = getenv("FRAME_VIEW")) {
            id<MTLTexture> of = !strcmp(part, "level") ? tex[frame::IMAGE1] : tex[frame::IMAGE3];
            const NSUInteger faces = of.textureType == MTLTextureTypeCube || of.textureType == MTLTextureTypeCubeArray
                                         ? 6
                                         : 1,
                             slices = std::max<NSUInteger>(of.arrayLength, 1) * faces;
            const bool level = !strcmp(part, "level"), array = !strcmp(part, "array");
            const NSUInteger layer = !strcmp(part, "slice") ? 1 : !strcmp(part, "face") ? 2 : 0;
            id<MTLTexture> view =
                [of newTextureViewWithPixelFormat:of.pixelFormat
                                      textureType:array ? MTLTextureType2DArray : MTLTextureType2D
                                           levels:level ? NSMakeRange(1, of.mipmapLevelCount - 1) : NSMakeRange(0, 1)
                                           slices:array ? NSMakeRange(0, slices) : NSMakeRange(layer, 1)];
            if (!view) {
                fprintf(stderr, "FRAME_VIEW %s: no view\n", part);
                return 1;
            }
            printf("VIEW %s %lux%lu levels %lu\n", part, (unsigned long)view.width, (unsigned long)view.height,
                   (unsigned long)view.mipmapLevelCount);
            if (getenv("FRAME_3D_CHECK") && !level && !array) {
                const NSUInteger row = of.width * 4, image = row * of.height;
                std::vector<uint8_t> back(image), want(image);
                for (size_t i = 0; i < image; i++)
                    want[i] = uint8_t((layer * image + i) * 13 + 5);
                [view getBytes:back.data()
                      bytesPerRow:row
                    bytesPerImage:image
                       fromRegion:MTLRegionMake2D(0, 0, of.width, of.height)
                      mipmapLevel:0
                            slice:0];
                printf("VIEW layer %lu %s\n", (unsigned long)layer, back == want ? "match" : "differs");
            }
            tex[frame::IMAGE3] = view;
        }
        id<MTLDepthStencilState> dss[frame::kDsSpecs];
        for (unsigned i = 0; i < frame::kDsSpecs; i++) {
            const frame::DsSpec spec = frame::dsSpec(i);
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunction(spec.depthCompare);
            dd.depthWriteEnabled = spec.depthWrite;
            MTLStencilDescriptor *faces[2] = {[MTLStencilDescriptor new], [MTLStencilDescriptor new]};
            for (int k = 0; k < 2; k++) {
                faces[k].stencilCompareFunction = MTLCompareFunction(spec.face[k][0]);
                faces[k].stencilFailureOperation = MTLStencilOperation(spec.face[k][1]);
                faces[k].depthFailureOperation = MTLStencilOperation(spec.face[k][2]);
                faces[k].depthStencilPassOperation = MTLStencilOperation(spec.face[k][3]);
                faces[k].readMask = spec.readMask[k];
                faces[k].writeMask = spec.writeMask[k];
            }
            dd.frontFaceStencil = faces[0];
            dd.backFaceStencil = faces[1];
            dss[i] = [dev newDepthStencilStateWithDescriptor:dd];
        }
        id<MTLSamplerState> smp[2];
        for (int s = 0; s < 2; s++) {
            frame::SamplerSpec sp = frame::sampler(s);
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = (MTLSamplerMinMagFilter)sp.minmag;
            sd.mipFilter = (MTLSamplerMipFilter)sp.mip;
            sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = (MTLSamplerAddressMode)sp.address;
            smp[s] = [dev newSamplerStateWithDescriptor:sd];
        }
        std::vector<double> cpu;
        std::vector<uint8_t> bytes;
        id<MTLBuffer> ring =
            [dev newBufferWithLength:frame::kRingBytes
                             options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        NSUInteger ringAt = 0;
        id<MTLBuffer> indices = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        id<MTLBuffer> zeros = [dev newBufferWithLength:frame::kZeroBytes options:MTLResourceStorageModeShared];
        memset(zeros.contents, 0, frame::kZeroBytes);
        // FRAME_SPLIT_PASS=mip: the extra pass renders into a texture of two levels instead (a twin-only attachment: refused at
        // the pass, pass.fmt, as Core Animation's average-luma pass into its mipmapped surface is)
        id<MTLTexture> mipTarget = nil;
        if (getenv("FRAME_SPLIT_PASS") && !strcmp(getenv("FRAME_SPLIT_PASS"), "mip")) {
            MTLTextureDescriptor *md =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex[frame::BLUR0].pixelFormat
                                                                   width:tex[frame::BLUR0].width
                                                                  height:tex[frame::BLUR0].height
                                                               mipmapped:YES];
            md.mipmapLevelCount = 2;
            md.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            md.storageMode = MTLStorageModePrivate;
            mipTarget = [dev newTextureWithDescriptor:md];
        }
        // FRAME_MSAA=<text>: every frame ends with a command buffer of one pass (NVK's resolve leaves its meta state to the draws
        // after it in its command buffer) drawing, three times, the first corpus state whose line holds <text> (iOS 17's Pride
        // poster lightsMesh, samples=2 RG11B10Float; FRAME_MSAA_CORPUS: the corpus, else the frame's) from the library at
        // FRAME_MSAA_LIB, into targets of its count (FRAME_MSAA_SAMPLES: of that many) and formats (FRAME_MSAA_FORMAT: target 0
        // of that MTLPixelFormat; FRAME_MSAA_DEPTH: the depth/stencil one), the colors cleared and not stored (FRAME_MSAA_STORE:
        // the first resolved into a texture of its size), the depth/stencil cleared. Every target is memoryless but a one-sample
        // state's colors, which are private, and that state's fragment textures are BLUR0.
        id<MTLRenderPipelineState> msaa = nil;
        id<MTLTexture> msaaTargets[8] = {}, msaaResolved = nil, msaaDepth = nil;
        bool msaaStencil = false, msaaMultisampled = false;
        if (const char *pick = getenv("FRAME_MSAA")) {
            NSError *libError = nil;
            id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(getenv("FRAME_MSAA_LIB") ?: "")]
                                                  error:&libError];
            std::string found;
            if (FILE *cf = fopen(getenv("FRAME_MSAA_CORPUS") ?: argv[4], "r")) {
                char buf[16384];
                while (found.empty() && fgets(buf, sizeof buf, cf)) {
                    std::string l(buf);
                    if (l.rfind("R ok=1 ", 0) || l.find(pick) == std::string::npos)
                        continue;
                    while (!l.empty() && l.back() == '\n')
                        l.pop_back();
                    for (size_t at; (at = l.find(" lib=")) != std::string::npos;)
                        l.erase(at, l.find(' ', at + 1) - at);
                    found = l;
                }
                fclose(cf);
            }
            MTLRenderPipelineDescriptor *md = lib && !found.empty() ? descriptor(lib, found) : nil;
            if (const char *n = getenv("FRAME_MSAA_SAMPLES"); n && md)
                md.rasterSampleCount = NSUInteger(atoi(n));
            if (const char *f = getenv("FRAME_MSAA_FORMAT"); f && md)
                md.colorAttachments[0].pixelFormat = MTLPixelFormat(atoi(f));
            if (const char *z = getenv("FRAME_MSAA_DEPTH"); z && md) {
                md.depthAttachmentPixelFormat = MTLPixelFormat(atoi(z));
                md.stencilAttachmentPixelFormat = md.depthAttachmentPixelFormat == MTLPixelFormatDepth32Float_Stencil8
                                                      ? MTLPixelFormatDepth32Float_Stencil8
                                                      : MTLPixelFormatInvalid;
            }
            msaa = md ? [dev newRenderPipelineStateWithDescriptor:md error:&libError] : nil;
            if (!msaa) {
                fprintf(stderr, "FRAME_MSAA %s: no state (%s)\n", pick,
                        libError ? libError.localizedDescription.UTF8String : "no line or library");
                return 1;
            }
            auto msTarget = [&](MTLPixelFormat f, bool memoryless) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor new];
                td.textureType = md.rasterSampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
                td.sampleCount = std::max<NSUInteger>(md.rasterSampleCount, 1);
                td.pixelFormat = f;
                td.width = 640;
                td.height = 360;
                td.usage = MTLTextureUsageRenderTarget;
                td.storageMode = memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
                return [dev newTextureWithDescriptor:td];
            };
            for (int c = 0; c < 8; c++)
                if (md.colorAttachments[c].pixelFormat != MTLPixelFormatInvalid)
                    msaaTargets[c] = msTarget(md.colorAttachments[c].pixelFormat, md.rasterSampleCount > 1);
            if (md.depthAttachmentPixelFormat != MTLPixelFormatInvalid)
                msaaDepth = msTarget(md.depthAttachmentPixelFormat, true);
            msaaStencil = md.stencilAttachmentPixelFormat != MTLPixelFormatInvalid;
            msaaMultisampled = md.rasterSampleCount > 1;
            if (getenv("FRAME_MSAA_STORE") && msaaTargets[0]) {
                MTLTextureDescriptor *rd =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:msaaTargets[0].pixelFormat
                                                                       width:640
                                                                      height:360
                                                                   mipmapped:NO];
                rd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                rd.storageMode = MTLStorageModePrivate;
                msaaResolved = [dev newTextureWithDescriptor:rd];
            }
            printf("MSAA %s samples %lu\n", found.substr(0, 120).c_str(), (unsigned long)md.rasterSampleCount);
        }
        auto encodeMsaa = [&](id<MTLCommandBuffer> cb, unsigned f) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            for (int c = 0; c < 8; c++) {
                if (!msaaTargets[c])
                    continue;
                rp.colorAttachments[c].texture = msaaTargets[c];
                rp.colorAttachments[c].loadAction = MTLLoadActionClear;
                rp.colorAttachments[c].storeAction = MTLStoreActionDontCare;
            }
            if (msaaResolved) {
                rp.colorAttachments[0].resolveTexture = msaaResolved;
                rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
            }
            if (msaaDepth) {
                rp.depthAttachment.texture = msaaDepth;
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.storeAction = MTLStoreActionDontCare;
            }
            if (msaaStencil) {
                rp.stencilAttachment.texture = msaaDepth;
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
            }
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:msaa];
            for (int slot = 0; slot < 8; slot++) {
                frame::Bytes u = {2, slot, shape.uniformBytes};
                frame::fill(bytes, u, f, slot);
                [e setVertexBytes:bytes.data() length:u.length atIndex:slot];
                [e setFragmentBytes:bytes.data() length:u.length atIndex:slot];
            }
            if (!msaaMultisampled)
                for (int slot = 0; slot < 16; slot++)
                    [e setFragmentTexture:tex[frame::BLUR0] atIndex:slot];
            for (int k = 0; k < 3; k++)
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 + k];
            [e endEncoding];
        };
        auto encodeExtra = [&](id<MTLCommandBuffer> cb, unsigned f) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = mipTarget ? mipTarget : tex[frame::BLUR0];
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[1].texture = tex[frame::BLUR_MEMORYLESS];
            rp.colorAttachments[1].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[1].storeAction = MTLStoreActionDontCare;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:extra[(f / 2) % extra.size()]];
            frame::Bytes vb = {1, 1, shape.vertexBytes};
            frame::fill(bytes, vb, f, 0);
            [e setVertexBytes:bytes.data() length:vb.length atIndex:1];
            for (int slot = 0; slot < 8; slot++) {
                frame::Bytes u = {2, slot, shape.uniformBytes};
                frame::fill(bytes, u, f, slot);
                [e setVertexBytes:bytes.data() length:u.length atIndex:slot == 1 ? 7 : slot];
                [e setFragmentBytes:bytes.data() length:u.length atIndex:slot];
            }
            // (FRAME_1D, FRAME_3D: slot 9 that texture, so m2v's segment samples the raw path's memory through its twin)
            for (int t = 0; t < 16; t++)
                [e setFragmentTexture:tex[t == 9 &&
                                                  (getenv("FRAME_1D") || getenv("FRAME_3D") || getenv("FRAME_ARRAY") ||
                                                   getenv("FRAME_CUBE") || getenv("FRAME_VIEW"))
                                              ? frame::IMAGE3
                                              : frame::IMAGE1]
                              atIndex:t];
            for (int s2 = 0; s2 < 16; s2++)
                [e setFragmentSamplerState:smp[s2 & 1] atIndex:s2];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [e endEncoding];
        };
        id<MTLBuffer> fills = nil;  // FRAME_FILL (frame.h)
        if (frame::fillUniforms()) {
            fills = [dev newBufferWithLength:frame::kFillBytes options:MTLResourceStorageModeShared];
            memset(fills.contents, 0, frame::kFillBytes);
        }
        // FRAME_MIP: IMAGE1's level 0 from a buffer of a pattern, then its levels (each frame, before its passes)
        id<MTLBuffer> mipSource = nil;
        if (frame::mipImage()) {
            const frame::TexDesc td = frame::texture(frame::IMAGE1);
            mipSource = [dev newBufferWithLength:NSUInteger(td.w) * td.h * 4 options:MTLResourceStorageModeShared];
            uint8_t *b = (uint8_t *)mipSource.contents;
            for (NSUInteger i = 0; i < NSUInteger(td.w) * td.h * 4; i++)
                b[i] = uint8_t(i * 131u + (i >> 9) * 7u + 3u);
        }
        // FRAME_YCBCR_WRITE=k: every k-th frame from the k-th, the 4:2:0 surface written under a write lock (its seed moves:
        // a decoder's next frame), so the raw path converts its image again then and only then (RMTexture _imageSeed)
        const unsigned ycbcrWrite = getenv("FRAME_YCBCR_WRITE") ? unsigned(atoi(getenv("FRAME_YCBCR_WRITE"))) : 0;
        const int settle = getenv("FRAME_COMPLETE_SETTLE") ? atoi(getenv("FRAME_COMPLETE_SETTLE")) : -1;
        framesRates(-1);
        for (unsigned f = 0; f < frames; f++) {
            if (int(f) == settle)
                usleep(2000000);
            if (ycbcrWrite && f && f % ycbcrWrite == 0 && surfaces[frame::IMAGE2] && getenv("FRAME_YCBCR")) {
                IOSurfaceLock(surfaces[frame::IMAGE2], 0, nullptr);
                uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(surfaces[frame::IMAGE2]);
                base[0] = uint8_t(base[0] + 1);
                IOSurfaceUnlock(surfaces[frame::IMAGE2], 0, nullptr);
            }
            @autoreleasepool {
                const uint64_t c0 = threadNs();
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                caCommandBuffer(cb);
                if (fills) {
                    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                    [b fillBuffer:fills
                            range:NSMakeRange(frame::kFillFrom, frame::kFillBytes - frame::kFillFrom)
                            value:frame::fillValue(f)];
                    [b endEncoding];
                }
                if (mipSource) {
                    const frame::TexDesc td = frame::texture(frame::IMAGE1);
                    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                    [b copyFromBuffer:mipSource
                               sourceOffset:0
                          sourceBytesPerRow:NSUInteger(td.w) * 4
                        sourceBytesPerImage:0
                                 sourceSize:MTLSizeMake(td.w, td.h, 1)
                                  toTexture:tex[frame::IMAGE1]
                           destinationSlice:0
                           destinationLevel:0
                          destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [b generateMipmapsForTexture:tex[frame::IMAGE1]];
                    [b endEncoding];
                }
                unsigned drawIndex = 0;
                for (const frame::Pass &pass : frame::passes(f, shape, blur.size(), uber.size())) {
                    if ((f & 1) && !extra.empty() && getenv("FRAME_SPLIT") && !pass.draws.empty() &&
                        !pass.draws[0].blur) {
                        // FRAME_SPLIT=cb: the same passes as command buffers of their own (what the segments must equal)
                        const bool own = !strcmp(getenv("FRAME_SPLIT"), "cb");
                        if (own) {
                            [cb commit];
                            cb = [queue commandBuffer];
                            caCommandBuffer(cb);
                        }
                        encodeExtra(cb, f);
                        if (own) {
                            [cb commit];
                            cb = [queue commandBuffer];
                            caCommandBuffer(cb);
                        }
                    }
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    for (int c = 0; c < 3; c++) {
                        if (pass.color[c] < 0)
                            continue;
                        rp.colorAttachments[c].texture = tex[pass.color[c]];
                        rp.colorAttachments[c].loadAction = pass.load[c] == 2   ? MTLLoadActionClear
                                                            : pass.load[c] == 1 ? MTLLoadActionLoad
                                                                                : MTLLoadActionDontCare;
                        rp.colorAttachments[c].storeAction =
                            frame::texture(pass.color[c]).memoryless ? MTLStoreActionDontCare : MTLStoreActionStore;
                        rp.colorAttachments[c].clearColor =
                            MTLClearColorMake(pass.clear[0], pass.clear[1], pass.clear[2], pass.clear[3]);
                    }
                    if (frame::rtSize() && !pass.draws.empty() && !pass.draws[0].blur) {
                        rp.renderTargetWidth = frame::texture(pass.color[0]).w - frame::kRtShortW;
                        rp.renderTargetHeight = frame::texture(pass.color[0]).h - frame::kRtShortH;
                    }
                    if (pass.depth >= 0) {
                        rp.depthAttachment.texture = rp.stencilAttachment.texture = tex[pass.depth];
                        rp.depthAttachment.loadAction = rp.stencilAttachment.loadAction = MTLLoadActionClear;
                        rp.depthAttachment.storeAction = rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
                        rp.depthAttachment.clearDepth = pass.clearDepth;
                        rp.stencilAttachment.clearStencil = pass.clearStencil;
                    }
                    // FRAME_VISIBILITY: every pass carries a visibility result buffer (GL-on-Metal's passes) and every draw sets the
                    // Disabled mode, so nothing counts into it
                    static id<MTLBuffer> visibility =
                        getenv("FRAME_VISIBILITY") ? [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]
                                                   : nil;
                    rp.visibilityResultBuffer = visibility;
                    // FRAME_SPI: GL-on-Metal's encoder SPI at Metal's defaults on every pass, and (facade) the first attachment's
                    // store action left Unknown and set by the encoder; the frontend has neither, so its passes go without
                    static const bool spi = getenv("FRAME_SPI") != nullptr;
#ifdef RAW_FACADE
                    const MTLStoreAction store = rp.colorAttachments[0].storeAction;
                    if (spi)
                        rp.colorAttachments[0].storeAction = MTLStoreActionUnknown;
#endif
                    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                    if (spi) {
                        id spiEncoder = e;
                        if ([spiEncoder respondsToSelector:@selector(setAlphaTestReferenceValue:)]) {
                            ((void (*)(id, SEL, float))objc_msgSend)(spiEncoder, @selector(setAlphaTestReferenceValue:),
                                                                     0.5f);
                            ((void (*)(id, SEL, BOOL))objc_msgSend)(spiEncoder, @selector(setViewportTransformEnabled:),
                                                                    YES);
                            ((void (*)(id, SEL, NSUInteger))objc_msgSend)(spiEncoder,
                                                                          @selector(setProvokingVertexMode:), 0);
                            ((void (*)(id, SEL, BOOL))objc_msgSend)(spiEncoder, @selector(setPrimitiveRestartEnabled:),
                                                                    YES);
                        }
#ifdef RAW_FACADE
                        [e setColorStoreAction:store atIndex:0];
#endif
                    }
                    for (int slot = 0; slot < frame::kBufferSlots; slot++) {
                        if (!frame::slotFilled(pass, 1, slot))
                            [e setVertexBuffer:zeros offset:0 atIndex:NSUInteger(slot)];
                        if (!frame::slotFilled(pass, 2, slot))
                            [e setFragmentBuffer:zeros offset:0 atIndex:NSUInteger(slot)];
                    }
                    bool bound[3][31] = {};
                    for (const frame::Draw &d : pass.draws) {
                        [e setRenderPipelineState:d.blur ? blur[d.state] : uber[d.state]];
                        if (visibility)
                            [e setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
                        for (const frame::Bytes &b : d.bytes) {
                            frame::fill(bytes, b, f, drawIndex);
                            if (shape.ring) {
                                if (ringAt + b.length > frame::kRingBytes)
                                    ringAt = 0;
                                memcpy((uint8_t *)ring.contents + ringAt, bytes.data(), b.length);
                                if (!bound[b.stage][b.slot]) {
                                    if (b.stage == 1)
                                        [e setVertexBuffer:ring offset:ringAt atIndex:b.slot];
                                    else
                                        [e setFragmentBuffer:ring offset:ringAt atIndex:b.slot];
                                    bound[b.stage][b.slot] = true;
                                } else if (b.stage == 1)
                                    [e setVertexBufferOffset:ringAt atIndex:b.slot];
                                else
                                    [e setFragmentBufferOffset:ringAt atIndex:b.slot];
                                // FRAME_RING_ALIGN: the ring's suballocation alignment (Core Animation's pool aligns to the
                                // device's minConstantBufferAlignmentBytes)
                                static const NSUInteger align =
                                    getenv("FRAME_RING_ALIGN") ? NSUInteger(atoi(getenv("FRAME_RING_ALIGN"))) : 256;
                                ringAt = (ringAt + b.length + align - 1) / align * align;
                                continue;
                            }
                            if (b.stage == 1)
                                [e setVertexBytes:bytes.data() length:b.length atIndex:b.slot];
                            else
                                [e setFragmentBytes:bytes.data() length:b.length atIndex:b.slot];
                        }
                        if (const int slot = fills ? frame::fillSlot(d) : -1; slot >= 0) {
                            [e setFragmentBuffer:fills offset:0 atIndex:NSUInteger(slot)];
                            bound[2][slot] = false;
                        }
                        for (int t = 0; t < 16; t++)
                            if (d.textures[t] >= 0)
                                [e setFragmentTexture:tex[d.textures[t]] atIndex:t];
                        for (int s = 0; s < 16; s++)
                            if (d.samplers[s] >= 0)
                                [e setFragmentSamplerState:smp[d.samplers[s]] atIndex:s];
                        if (getenv("FRAME_SAMPLER_SLOT"))
                            [e setFragmentSamplerState:smp[0] atIndex:atoi(getenv("FRAME_SAMPLER_SLOT"))];
                        // FRAME_SPI: the first bound sampler again with a bind's LOD clamps (GL-on-Metal's)
                        if (spi)
                            for (int s = 0; s < 16; s++)
                                if (d.samplers[s] >= 0) {
                                    [e setFragmentSamplerState:smp[d.samplers[s]]
                                                   lodMinClamp:0.5f
                                                   lodMaxClamp:3.25f
                                                       atIndex:s];
                                    break;
                                }
                        [e setScissorRect:(MTLScissorRect){d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]}];
                        if (d.ds >= 0) {
                            [e setDepthStencilState:dss[d.ds]];
                            [e setStencilFrontReferenceValue:d.stencilRef[0] backReferenceValue:d.stencilRef[1]];
                        }
                        if (d.raster) {
                            [e setCullMode:MTLCullMode(d.cull)];
                            [e setFrontFacingWinding:MTLWinding(d.winding)];
                            [e setTriangleFillMode:MTLTriangleFillMode(d.fill)];
                            [e setDepthBias:d.bias[0] slopeScale:d.bias[1] clamp:d.bias[2]];
                        }
                        if (frame::indexedDraw(d.blur, drawIndex)) {
                            static uint32_t ibAt = 0;
                            const frame::Indexed x =
                                frame::indexed(drawIndex, static_cast<uint8_t *>(indices.contents), ibAt);
                            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                          indexCount:x.count
                                           indexType:MTLIndexTypeUInt16
                                         indexBuffer:indices
                                   indexBufferOffset:x.offset
                                       instanceCount:x.instances
                                          baseVertex:x.baseVertex
                                        baseInstance:x.baseInstance];
                        } else
                            [e drawPrimitives:(MTLPrimitiveType)d.primitive
                                  vertexStart:d.first
                                  vertexCount:d.count
                                instanceCount:d.instances
                                 baseInstance:d.baseInstance];
                        drawIndex++;
                    }
                    [e endEncoding];
                }
                // FRAME_EXTRA: every odd frame, a second command buffer of one pass drawing an extra state (m2v's, on the facade)
                // into BLUR0, then the next frame's raw work reads BLUR0 (its blur passes sample it). FRAME_SPLIT: that pass in the
                // same command buffer, before the main pass (the facade submits it in segments: raw, m2v, raw)
                if ((f & 1) && !extra.empty() && !getenv("FRAME_SPLIT")) {
                    [cb commit];
                    cb = [queue commandBuffer];
                    caCommandBuffer(cb);
                    encodeExtra(cb, f);
                }
                if (msaa) {
                    [cb commit];
                    cb = [queue commandBuffer];
                    caCommandBuffer(cb);
                    encodeMsaa(cb, f);
                }
                [cb commit];
                cpu.push_back((threadNs() - c0) / 1e6);
                [cb waitUntilCompleted];
                if (cb.error) {
                    fprintf(stderr, "frame %u: %s\n", f, cb.error.localizedDescription.UTF8String);
                    return 1;
                }
                framesRates(f);
            }
        }
        const size_t warm = cpu.size() > 20 ? 10 : 0;
        std::vector<double> c(cpu.begin() + warm, cpu.end());
        std::sort(c.begin(), c.end());
        double sum = 0;
        for (double x : c)
            sum += x;
        if (!c.empty())
            printf(FRAME_PATH " frames=%zu blur=%u draws=%u: encode cpu mean %.3f ms p50 %.3f p90 %.3f\n", c.size(),
                   shape.blurPasses, shape.draws, sum / c.size(), c[c.size() / 2], c[c.size() * 9 / 10]);
#ifdef RAW_FACADE
        {
            char rates[2048];
            if (raw_metal_rates(rates, sizeof rates))
                printf("RATES %s\n", rates);
        }
#endif
        pushcount_report("nvkframe");
    }
    return 0;
}
