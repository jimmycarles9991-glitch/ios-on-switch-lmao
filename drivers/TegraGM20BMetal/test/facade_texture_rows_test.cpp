// SPDX-License-Identifier: MIT
// Textures over a buffer's or a surface's rows as Metal lays them out, made by the driver or nil as Metal answers, with no
// m2v device: rows past their memory, a buffer texture at 16 bytes (a pool image), and a surface plane off 16 (Metal's GPU
// layout, its CPU rows its own).
#include "facade_harness.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <sstream>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

NSURL *fileURL(const std::string &path)
{
    return [NSURL fileURLWithPath:@(path.c_str())];
}

// A texture whose rows run past its buffer's or surface's memory, or a memoryless one over a buffer, is nil as Metal
// answers an invalid descriptor
void texturesPastTheirMemory()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:0 bytesPerRow:256], nil) << "16 KiB of rows in 4 KiB";
    d.height = 16;
    d.storageMode = MTLStorageModeMemoryless;
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:0 bytesPerRow:256], nil);
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(surface != nullptr);
    d.storageMode = MTLStorageModeShared;
    d.height = 4096;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:surface plane:0], nil) << "rows past the allocation";
    d.height = 16;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:surface plane:1], nil) << "a plane it does not have";
    CFRelease(surface);
    IOSurfaceRef narrow = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @60,
        (id)kIOSurfaceHeight : @16,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @248,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(narrow != nullptr);
    ASSERT_EQ(IOSurfaceGetBytesPerRow(narrow) & 15, 8u) << "rows 8 past a multiple of 16";
    d.width = 60;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:narrow plane:0], nil) << "Metal asserts on rows off 16";
    CFRelease(narrow);
}

// A texture over a buffer at an offset and rows 16 but not 32 bytes apart (boot 21's SwitchHWTest, 12x8 at offset 48) is
// sampled through a pool image the copy engine fills before the draw; one off 16, which Metal's validation stops, is nil
void bufferTexturesAt16()
{
    const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, {sampled}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:12
                                                                                height:8
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:d offset:48 bytesPerRow:48];
    ASSERT_TRUE(texture != nil);
    uint8_t texel[4] = {1, 2, 3, 4};
    [texture replaceRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0 withBytes:texel bytesPerRow:4];
    EXPECT_EQ(memcmp(static_cast<uint8_t *>(buffer.contents) + 48 + 3 * 48 + 5 * 4, texel, 4), 0)
        << "its rows the buffer's";
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    EXPECT_EQ(drawSampling(device, state, texture, sampler).status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:56 bytesPerRow:48], nil) << "an offset off 16";
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:48 bytesPerRow:56], nil) << "rows off 16";
}

// A surface plane whose offset or rows are off 16 (any covering its row, as IOSurface makes them): Metal's GPU reads it from
// both rounded down to 16 and its CPU at the plane's own (m2v linear_plan.rs, measured), so the raw texture is made at the
// rounded layout and replaceRegion and getBytes take the plane's; rows that overlap once rounded are read through a pool of
// the rounded rows (plain_rows.h)
void planesOff16()
{
    const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, {sampled}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    const auto plane = [](uint32_t width, uint32_t offset, uint32_t row) {
        return @{
            (id)kIOSurfacePlaneWidth : @(width),
            (id)kIOSurfacePlaneHeight : @8,
            (id)kIOSurfacePlaneBytesPerElement : @4,
            (id)kIOSurfacePlaneBytesPerRow : @(row),
            (id)kIOSurfacePlaneOffset : @(offset),
            (id)kIOSurfacePlaneSize : @(row * 8)
        };
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @12,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA')),
        (id)kIOSurfaceAllocSize : @4096,
        (id)kIOSurfacePlaneInfo : @[ plane(12, 0, 48), plane(12, 388, 52), plane(13, 1024, 60) ]
    });
    ASSERT_TRUE(surface != nullptr);
    ASSERT_EQ(IOSurfaceGetPlaneCount(surface), 3u);
    ASSERT_EQ(IOSurfaceGetBytesPerRowOfPlane(surface, 1), 52u);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:12
                                                                                height:8
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d iosurface:surface plane:1];
    ASSERT_TRUE(texture != nil);
    const uint8_t texel[4] = {1, 2, 3, 4};
    [texture replaceRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0 withBytes:texel bytesPerRow:4];
    const uint8_t *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(surface));
    EXPECT_EQ(memcmp(base + 388 + 3 * 52 + 5 * 4, texel, 4), 0) << "the CPU writes the plane's own rows";
    uint8_t back[4] = {};
    [texture getBytes:back bytesPerRow:4 fromRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0];
    EXPECT_EQ(memcmp(back, texel, 4), 0);
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    EXPECT_EQ(drawSampling(device, state, texture, sampler).status, MTLCommandBufferStatusCompleted);
    // rows of 60 overlap once 48 apart: an M4 Max makes this texture, a kernel reading texel (x, y) at 1024 + 48y + 4x and
    // getBytes at 1024 + 60y + 4x (the off-16 rows probe on this surface: 104 of 104 texels each, 3 runs and the
    // validation layer alike)
    d.width = 13;
    SwitchMetalTexture *overlapping =
        (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:2];
    ASSERT_TRUE(overlapping != nil && overlapping->_gm20bOk);
    const gm20b::Texture &x = overlapping->_raw;
    EXPECT_EQ(x.pitch, 48u) << "the GPU's rows";
    EXPECT_EQ(x.va - x.import->bo.va, 1024u);
    EXPECT_TRUE(x.sampled && x.storeView) << "read through its pool, as no header lays out rows that overlap";
    uint8_t *rows = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(surface)) + 1024;
    for (uint32_t i = 0; i < 60 * 8; i++)
        rows[i] = uint8_t(i * 7 + 1);
    uint8_t read[8][52];
    [(id<MTLTexture>)overlapping getBytes:read bytesPerRow:52 fromRegion:MTLRegionMake2D(0, 0, 13, 8) mipmapLevel:0];
    for (uint32_t y = 0; y < 8; y++)
        EXPECT_EQ(memcmp(read[y], rows + 60 * y, 52), 0) << "getBytes row " << y << " at the plane's rows";
    CFRelease(surface);
}

// A surface over the client's memory may start off a page: its texture is imported from that page, its GPU address as far
// into the page as the surface's base and its CPU rows the surface's (Device::createSurfaceTexture, Kernel::bindImport)
void surfaceOffAPage()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const size_t page = size_t(getpagesize());
    void *pages = nullptr;
    ASSERT_EQ(posix_memalign(&pages, page, 3 * page), 0);
    uint8_t *base = static_cast<uint8_t *>(pages) + 256;
    gm20b::Texture t;
    ASSERT_TRUE(gRM->raw->createSurfaceTexture(0, base, 64 * 64 * 4, nullptr, 0, 64, 64, 256,
                                               uint32_t(MTLPixelFormatBGRA8Unorm), t, false));
    ASSERT_TRUE(t.import != nullptr);
    EXPECT_EQ(t.cpu, base);
    EXPECT_EQ(t.va - t.import->bo.vaBase, 256u) << "the GPU reads from the surface's base, not its page's";
    gRM->raw->destroyTexture(t);
    free(pages);
}

// A texture over a surface whose import the kernel refuses is nil, with no m2v device asked for one: m2v's NVK imports the
// same pages through the same BoImport (boot C32's mediaanalysisd: a 360x360 BGRA surface it reads and writes)
void aRefusedImportIsNil()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @360,
        (id)kIOSurfaceHeight : @360,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @1472,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(surface != nullptr);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:360
                                                                                height:360
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    kernel_model::model.refuseImports = 1;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:surface plane:0], nil);
    EXPECT_EQ(kernel_model::model.refuseImports, 0u) << "the kernel was asked";
    EXPECT_TRUE([device newTextureWithDescriptor:d iosurface:surface plane:0] != nil) << "the next import is taken";
    CFRelease(surface);
}

// BGR10_XR_sRGB and BGRA10_XR_sRGB are raw's: AegirPoster's 720x1280 shared target that it samples (boot C32 made it on m2v
// and replayed every pass into it there), and the same of BGRA10_XR_sRGB, each with the stand-in a draw samples
void anSrgbExtendedRangeTextureIsRaws()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_bgr10_srgb_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.xr.xr_bgra10_srgb_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    for (const MTLPixelFormat format : {MTLPixelFormatBGR10_XR_sRGB, MTLPixelFormatBGRA10_XR_sRGB}) {
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:720 height:1280 mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d];
        ASSERT_TRUE(t != nil) << format;
        EXPECT_TRUE(t->_gm20bOk && t->_raw.standIn) << format;
    }
}

// A heap's size for an extended-range array and an RG8Unorm_sRGB texture, which the device makes beside its arena, is the
// raw path's footprint of the texture and the stand-in a draw samples, with no m2v device asked
void heapSizesOfStandInTextures()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray;
    d.pixelFormat = MTLPixelFormatBGR10_XR;
    d.width = 64;
    d.height = 32;
    d.arrayLength = 3;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead;
    EXPECT_GE([device heapTextureSizeAndAlignWithDescriptor:d].size, NSUInteger(64 * 32 * 3 * (4 + 8)))
        << "its codes and its half stand-in";
    d.textureType = MTLTextureType2D;
    d.arrayLength = 1;
    d.pixelFormat = MTLPixelFormatRG8Unorm_sRGB;
    EXPECT_GE([device heapTextureSizeAndAlignWithDescriptor:d].size, NSUInteger(64 * 32 * (2 + 4)));
}

size_t jobCount()
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    return kernel_model::model.jobs.size();
}

// The copy engine's methods of the jobs since `first`, but the address of their staging rows (OFFSET_IN of a write,
// OFFSET_OUT of a read): the copy as it names the texture's memory. A write's staging rows hold `texel`.
using CopyMethods = std::vector<std::pair<uint32_t, uint32_t>>;

CopyMethods texelCopy(size_t first, bool write, const uint8_t *texel)
{
    constexpr uint32_t kOffsetInUpper = 0x0400, kOffsetOutUpper = 0x0408;  // NVB0B5_OFFSET_IN_UPPER, _OUT_UPPER
    const uint32_t staging = write ? kOffsetInUpper : kOffsetOutUpper;
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    const std::vector<kernel_model::Job> &jobs = kernel_model::model.jobs;
    CopyMethods out;
    uint64_t rows = 0;
    for (size_t j = first; j < jobs.size(); j++) {
        const std::vector<uint32_t> &w = jobs[j].words;
        std::vector<switch_gm20b_method> m(w.size());
        const long n = switch_gm20b_push_decode(w.data(), w.size(), m.data(), m.size());
        EXPECT_GE(n, 0);
        for (long i = 0; i < n; i++) {
            if (m[i].method == staging || m[i].method == staging + 4)
                rows = rows << 32 | m[i].value;
            else
                out.push_back({m[i].method, m[i].value});
        }
    }
    if (write) {
        const uint8_t *staged = kernel_model::model.host(rows, 4);
        EXPECT_TRUE(staged && !memcmp(staged, texel, 4)) << "the rows the copy engine copies into the texture";
    }
    return out;
}

// `t`'s replaceRegion of `texel` at `at` of `level` and `slice`, then its getBytes there: their copies
std::pair<CopyMethods, CopyMethods> texelCopies(id<MTLTexture> t, NSUInteger level, NSUInteger slice, MTLOrigin at,
                                                const uint8_t *texel)
{
    const MTLRegion r = MTLRegionMake2D(at.x, at.y, 1, 1);
    size_t first = jobCount();
    [t replaceRegion:r mipmapLevel:level slice:slice withBytes:texel bytesPerRow:4 bytesPerImage:0];
    const CopyMethods written = texelCopy(first, true, texel);
    uint8_t back[4] = {};
    first = jobCount();
    [t getBytes:back bytesPerRow:4 bytesPerImage:0 fromRegion:r mipmapLevel:level slice:slice];
    return {written, texelCopy(first, false, nullptr)};
}

// A view's CPU copies at its level and slice are its texture's at levels.location and slices.location on, as an M4 Max
// takes them (stack-fix probe/viewcpu.mm): a view of levels 1 and 2 of a Shared mipmapped texture, and of slices 1 and 2
// of an array's, of another format and of its own, written and read at two texels. The kernel model runs no copy engine,
// so no bytes come back: the view's copy is the copy the texture's own makes there and no other level's or slice's.
void aViewsCpuCopiesTakeItsTexturesPaths()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    for (const NSUInteger layers : {1u, 3u}) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:64
                                                                                    height:32
                                                                                 mipmapped:YES];
        if (layers > 1) {
            d.textureType = MTLTextureType2DArray;
            d.arrayLength = layers;
        }
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        id<MTLTexture> texture = [device newTextureWithDescriptor:d];
        ASSERT_TRUE(texture != nil) << layers;
        const NSRange slices = layers > 1 ? NSMakeRange(1, 2) : NSMakeRange(0, 1);
        for (const MTLPixelFormat format : {MTLPixelFormatRGBA8Uint, MTLPixelFormatRGBA8Unorm}) {
            SwitchMetalTexture *view = (SwitchMetalTexture *)[texture newTextureViewWithPixelFormat:format
                                                                                        textureType:d.textureType
                                                                                             levels:NSMakeRange(1, 2)
                                                                                             slices:slices];
            ASSERT_TRUE(view != nil && view->_gm20bOk) << format << " layers " << layers;
            for (NSUInteger slice = 0; slice < slices.length; slice++)
                for (NSUInteger level : {0u, 1u})
                    for (const MTLOrigin at :
                         {MTLOriginMake(1, 1, 0), MTLOriginMake(level ? 13 : 29, level ? 6 : 14, 0)}) {
                        const NSUInteger ownSlice = slices.location + slice;
                        const uint8_t texel[4] = {uint8_t(1 + level), uint8_t(format), uint8_t(at.x), uint8_t(at.y)};
                        const auto viewed = texelCopies((id<MTLTexture>)view, level, slice, at, texel);
                        const auto own = texelCopies(texture, 1 + level, ownSlice, at, texel);
                        const auto levelNumber = texelCopies(texture, level, ownSlice, at, texel);
                        std::ostringstream where;
                        where << "format " << format << " layers " << layers << " level " << level << " slice " << slice
                              << " at " << at.x << "," << at.y;
                        EXPECT_FALSE(viewed.first.empty() || viewed.second.empty()) << where.str();
                        EXPECT_EQ(viewed.first, own.first) << where.str();
                        EXPECT_EQ(viewed.second, own.second) << where.str();
                        EXPECT_NE(viewed.first, levelNumber.first) << where.str() << ": the texture's level " << level;
                        EXPECT_NE(viewed.second, levelNumber.second) << where.str();
                        if (ownSlice == slice)
                            continue;
                        const auto sliceNumber = texelCopies(texture, 1 + level, slice, at, texel);
                        EXPECT_NE(viewed.first, sliceNumber.first) << where.str() << ": the texture's slice " << slice;
                        EXPECT_NE(viewed.second, sliceNumber.second) << where.str();
                    }
        }
    }
}

// An extended-range texture views as its sRGB twin on the driver, and as a format of its texel size not at all, as Metal
// answers (m2v view_plan.rs, measured: plain Metal aborts the other pairs); a view of a part of an X32_Stencil8 texture,
// a device texture buffer and a heap's sizes for shapes no heap holds are raw's answers too
void viewsAndShapesAnsweredOnRaw()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_bgr10_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.xr.xr_bgr10_srgb_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGR10_XR
                                                                                 width:64
                                                                                height:32
                                                                             mipmapped:YES];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    ASSERT_TRUE(texture != nil);
    SwitchMetalTexture *srgb =
        (SwitchMetalTexture *)[texture newTextureViewWithPixelFormat:MTLPixelFormatBGR10_XR_sRGB];
    ASSERT_TRUE(srgb != nil && srgb->_gm20bOk);
    EXPECT_TRUE(srgb->_raw.standIn != nullptr);
    SwitchMetalTexture *level = (SwitchMetalTexture *)[texture newTextureViewWithPixelFormat:MTLPixelFormatBGR10_XR_sRGB
                                                                                 textureType:MTLTextureType2D
                                                                                      levels:NSMakeRange(1, 1)
                                                                                      slices:NSMakeRange(0, 1)];
    ASSERT_TRUE(level != nil && level->_gm20bOk);
    EXPECT_EQ([texture newTextureViewWithPixelFormat:MTLPixelFormatR32Uint], nil) << "Metal aborts it";
    MTLTextureDescriptor *stencil = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatX32_Stencil8
                                                                                       width:32
                                                                                      height:32
                                                                                   mipmapped:NO];
    stencil.storageMode = MTLStorageModePrivate;
    stencil.usage = MTLTextureUsageShaderRead;
    SwitchMetalTexture *x32 = (SwitchMetalTexture *)[device newTextureWithDescriptor:stencil];
    ASSERT_TRUE(x32 != nil && x32->_gm20bOk);
    EXPECT_EQ(((id<MTLTexture>)x32).pixelFormat, MTLPixelFormatX32_Stencil8);
    MTLTextureDescriptor *line =
        [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:100
                                                     resourceOptions:0
                                                               usage:MTLTextureUsageShaderRead];
    SwitchMetalTexture *buffer = (SwitchMetalTexture *)[device newTextureWithDescriptor:line];
    ASSERT_TRUE(buffer != nil && buffer->_gm20bOk);
    EXPECT_EQ(buffer->_raw.type, gm20b::fx::kTexOneDBuffer);
    EXPECT_EQ([device heapTextureSizeAndAlignWithDescriptor:line].size, 400u);
    d.storageMode = MTLStorageModeMemoryless;
    EXPECT_EQ([device heapTextureSizeAndAlignWithDescriptor:d].size, 0u) << "no heap holds a memoryless texture";
    MTLTextureDescriptor *bfloat =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(116) width:16 height:16 mipmapped:NO];
    EXPECT_EQ([device newTextureWithDescriptor:bfloat], nil) << "no bfloat16 component in the GM20B";
    EXPECT_EQ([device heapTextureSizeAndAlignWithDescriptor:bfloat].size, 0u);
}

}  // namespace

TEST(GM20BFacadeTextureRows, ViewsAndShapesAreAnsweredOnRawWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(viewsAndShapesAnsweredOnRaw);
}

TEST(GM20BFacadeTextureRows, AViewsCpuCopiesTakeItsTexturesPathsWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(aViewsCpuCopiesTakeItsTexturesPaths);
}

TEST(GM20BFacadeTextureRows, AHeapSizesAStandInTextureWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(heapSizesOfStandInTextures);
}

TEST(GM20BFacadeTextureRows, ARefusedSurfaceImportIsNilWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(aRefusedImportIsNil);
}

TEST(GM20BFacadeTextureRows, AnSrgbExtendedRangeTextureIsRawsWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(anSrgbExtendedRangeTextureIsRaws);
}

TEST(GM20BFacadeTextureRows, ATexturePastItsMemoryOrOffItsRowsIsNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(texturesPastTheirMemory);
}

TEST(GM20BFacadeTextureRows, ABufferTextureAt16IsSampledThroughAPoolImage)
{
    switch_ios::test::ExpectPassesInFreshProcess(bufferTexturesAt16);
}

TEST(GM20BFacadeTextureRows, APlaneOff16IsMadeAtMetalsGpuLayoutWithItsCpuRows)
{
    switch_ios::test::ExpectPassesInFreshProcess(planesOff16);
}

TEST(GM20BFacadeTextureRows, ASurfaceOffAPageIsImportedFromItsPage)
{
    switch_ios::test::ExpectPassesInFreshProcess(surfaceOffAPage);
}
