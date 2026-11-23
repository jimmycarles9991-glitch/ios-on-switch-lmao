// SPDX-License-Identifier: MIT
// Every texture made over a surface, over a buffer or in a heap ends on the driver: made there, or nil as Metal, the identity
// or GM20B answers, with no m2v device opened. An M4 Max's answers (m2v-textures3 probes/surfaces.mm, xrsurf.mm, keeps.mm;
// m2v-textures4 probes/forms.mm, reads.mm, more.mm, tb.mm).
#include "facade_harness.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>

namespace {

using namespace harness;

constexpr NSUInteger kSide = 64;

IOSurfaceRef chunky(uint32_t fourcc, NSUInteger bytes)
{
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfaceBytesPerElement : @(bytes),
        (id)kIOSurfacePixelFormat : @(fourcc)
    });
}

IOSurfaceRef twoPlanes()
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(kSide),
            (id)kIOSurfacePlaneHeight : @(kSide),
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(kSide * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : kSide * kSide * 4),
            (id)kIOSurfacePlaneSize : @(kSide * kSide * bytes)
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(kSide * kSide * 5),
        (id)kIOSurfacePlaneInfo : planes
    });
}

MTLTextureDescriptor *flat(uint32_t format, MTLTextureUsage usage, NSUInteger side = kSide)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                 width:side
                                                                                height:side
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = usage;
    return d;
}

std::vector<std::pair<std::string, MTLPixelFormat>> conversions()
{
    return {{"raw.xr.xr_bgr10_frag", MTLPixelFormatRGBA16Float},
            {"raw.xr.xr_bgra10_frag", MTLPixelFormatRGBA16Float},
            {"raw.xr.xr_bgr10_srgb_frag", MTLPixelFormatRGBA16Float},
            {"raw.xr.xr_bgra10_srgb_frag", MTLPixelFormatRGBA16Float},
            {"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
            {"raw.xr.xr_two_planes_srgb_frag", MTLPixelFormatRGBA16Float},
            {"raw.xr.xr_planes_merge_frag", MTLPixelFormatRG32Uint},
            {"raw.xr.xr_planes_split_frag", MTLPixelFormatR32Uint}};
}

// A texel buffer of each extended-range format, over a buffer and the device's own, is raw's with the stand-in a draw
// samples; RGB10A8_2P_XR10's holds its alpha bytes 16 KiB past its words, which its heap size counts (an M4 Max: 16,400
// bytes for 16 texels)
void extendedRangeTexelBuffers()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", conversions()));
    ASSERT_TRUE(device != nil);
    id<MTLBuffer> buffer = [device newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    for (uint32_t format = 550; format <= 555; format++) {
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                   width:100
                                                         resourceOptions:0
                                                                   usage:MTLTextureUsageShaderRead];
        SwitchMetalTexture *t = (SwitchMetalTexture *)[buffer newTextureWithDescriptor:d offset:256 bytesPerRow:800];
        ASSERT_TRUE(t != nil) << format;
        EXPECT_TRUE(t->_gm20bOk && t->_raw.standIn) << format;
        EXPECT_EQ(t->_raw.type, gm20b::fx::kTexOneDBuffer) << format;
        SwitchMetalTexture *own = (SwitchMetalTexture *)[device newTextureWithDescriptor:d];
        ASSERT_TRUE(own != nil && own->_gm20bOk && own->_raw.standIn) << format;
    }
    MTLTextureDescriptor *planes =
        [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormat(550)
                                                               width:16
                                                     resourceOptions:0
                                                               usage:MTLTextureUsageShaderRead];
    EXPECT_EQ([device heapTextureSizeAndAlignWithDescriptor:planes].size, 16400u);
}

// Over a surface: a texture buffer of the plane's bytes, and RGB10A8_2P_XR10 over a two-plane surface of Unknown usage, for
// a format view or smaller than the surface, are raw's; over a one-plane surface it reads BGR10_XR's words and alpha 0
// (its stand-in read through a swizzle of alpha zero); other types, depth formats and formats no donor GPU samples are nil
void surfaceShapesEndOnRaw()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", conversions()));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef bgra = chunky('BGRA', 4), half = chunky('RGhA', 8), planes = twoPlanes();
    ASSERT_TRUE(bgra && half && planes);
    MTLTextureDescriptor *line =
        [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:kSide
                                                     resourceOptions:0
                                                               usage:MTLTextureUsageShaderRead];
    SwitchMetalTexture *buffer = (SwitchMetalTexture *)[device newTextureWithDescriptor:line iosurface:bgra plane:0];
    ASSERT_TRUE(buffer != nil && buffer->_gm20bOk);
    EXPECT_EQ(buffer->_raw.type, gm20b::fx::kTexOneDBuffer);
    // no longer than the surface's rows, which an M4 Max's Metal asserts on, counting 2 bytes a texel of RGB10A8_2P_XR10
    line.width = kSide + 1;
    EXPECT_EQ([device newTextureWithDescriptor:line iosurface:bgra plane:0], nil) << "a texel past a row of 256 bytes";
    IOSurfaceRef xrWords = chunky('w30r', 4);
    ASSERT_TRUE(xrWords);
    line.pixelFormat = MTLPixelFormat(550);
    line.width = 2 * kSide;
    SwitchMetalTexture *words = (SwitchMetalTexture *)[device newTextureWithDescriptor:line iosurface:xrWords plane:0];
    EXPECT_TRUE(words != nil && words->_gm20bOk) << "128 texels of 550 over a row of 256 bytes";
    line.width = 2 * kSide + 1;
    EXPECT_EQ([device newTextureWithDescriptor:line iosurface:xrWords plane:0], nil) << "129 texels of 550";
    CFRelease(xrWords);
    for (const MTLTextureUsage usage : {MTLTextureUsage(MTLTextureUsageUnknown),
                                        MTLTextureUsage(MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView)})
        for (const NSUInteger side : {kSide, kSide / 2}) {
            SwitchMetalTexture *xr =
                (SwitchMetalTexture *)[device newTextureWithDescriptor:flat(550, usage, side) iosurface:planes plane:0];
            ASSERT_TRUE(xr != nil && xr->_gm20bOk) << usage << " " << side;
            EXPECT_TRUE(xr->_raw.xrPlanes) << usage << " " << side;
        }
    for (const uint32_t format : {550u, 551u}) {
        SwitchMetalTexture *words =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:flat(format, MTLTextureUsageShaderRead)
                                                         iosurface:half
                                                             plane:0];
        ASSERT_TRUE(words != nil && words->_gm20bOk && words->_raw.standIn) << format;
        EXPECT_EQ(words->_raw.format, format == 550 ? 554u : 555u) << format;
        EXPECT_EQ(words->_raw.standIn->swizzle[3], uint8_t(MTLTextureSwizzleZero)) << "alpha 0";
    }
    MTLTextureDescriptor *array = flat(MTLPixelFormatBGRA8Unorm, MTLTextureUsageShaderRead);
    array.textureType = MTLTextureType2DArray;
    EXPECT_EQ([device newTextureWithDescriptor:array iosurface:bgra plane:0], nil) << "Metal's validation stop";
    EXPECT_EQ([device newTextureWithDescriptor:flat(252, MTLTextureUsageShaderRead) iosurface:bgra plane:0], nil);
    EXPECT_EQ([device newTextureWithDescriptor:flat(563, MTLTextureUsageShaderRead) iosurface:half plane:0], nil);
    EXPECT_EQ([device newTextureWithDescriptor:flat(160, MTLTextureUsageShaderRead) iosurface:bgra plane:0], nil);
    for (IOSurfaceRef s : {bgra, half, planes})
        CFRelease(s);
}

MTLTextureDescriptor *turned(uint32_t format, NSUInteger rotation, NSUInteger width, NSUInteger height)
{
    MTLTextureDescriptor *d = flat(format, MTLTextureUsageRenderTarget);
    d.width = width;
    d.height = height;
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(d, sel_registerName("setRotation:"), rotation);
    return d;
}

// A 4:2:0 surface of kSide x kSide (CoreVideo's '420v' planes)
IOSurfaceRef fourTwoZero()
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned plane : {0u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(plane ? kSide / 2 : kSide),
            (id)kIOSurfacePlaneHeight : @(plane ? kSide / 2 : kSide),
            (id)kIOSurfacePlaneBytesPerElement : @(plane ? 2 : 1),
            (id)kIOSurfacePlaneBytesPerRow : @(kSide),
            (id)kIOSurfacePlaneOffset : @(plane ? kSide * kSide : 0),
            (id)kIOSurfacePlaneSize : @(plane ? kSide * kSide / 2 : kSide * kSide)
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('420v')),
        (id)kIOSurfaceAllocSize : @(kSide * kSide * 3 / 2),
        (id)kIOSurfacePlaneInfo : planes
    });
}

// An M4 Max renders into a YCbCr surface texture of any usage as into a render target (harness/ycbcrnorenderoracle.mm): one
// made to be read alone holds the memoryless stand-in and the store shape its passes draw through
void aReadOnlyYcbcrTextureIsATarget()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.ycbcr_store.ycbcr_store_luma8_frag", MTLPixelFormatR8Unorm},
                                           {"raw.ycbcr_store.ycbcr_store_chroma8_frag", MTLPixelFormatRG8Unorm}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = fourTwoZero();
    ASSERT_TRUE(s);
    for (const MTLTextureUsage usage : {MTLTextureUsage(MTLTextureUsageShaderRead),
                                        MTLTextureUsage(MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget)}) {
        SwitchMetalTexture *t =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:flat(500, usage) iosurface:s plane:0];
        ASSERT_TRUE(t != nil && t->_gm20bOk) << usage;
        EXPECT_TRUE(t->_raw.planarStandIn && t->_raw.planarStandIn->memoryless) << usage;
        EXPECT_NE(t->_raw.ycbcrStore, nullptr) << usage;
    }
    CFRelease(s);
}

// Every format Metal renders turns alike (an M4 Max: m2v-textures4 probes/reads.mm, more.mm landings): one the GM20B
// renders none of (drawn turned into its stand-in), an extended-range one, and a plane of several as that plane alone. Metal
// makes no rotated block-compressed texture
void everyRenderedFormatTurns()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", conversions()));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef two = chunky('RG08', 2), words = chunky('w30r', 4), half = chunky('RGhA', 8), planes = fourTwoZero();
    ASSERT_TRUE(two && words && half && planes);

    const struct {
        IOSurfaceRef s;
        uint32_t format;
        NSUInteger rotation, plane, side;
    } kTurned[] = {{two, 31, 1, 0, kSide},   {half, 553, 3, 0, kSide},  {words, 554, 2, 0, kSide},
                   {half, 552, 4, 0, kSide}, {planes, 10, 1, 0, kSide}, {planes, 30, 3, 1, kSide / 2}};

    for (const auto &c : kTurned) {
        SwitchMetalTexture *t =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:turned(c.format, c.rotation, c.side, c.side)
                                                         iosurface:c.s
                                                             plane:c.plane];
        ASSERT_TRUE(t != nil && t->_gm20bOk) << c.format;
        EXPECT_EQ(t->_raw.rotation, c.rotation) << c.format;
        EXPECT_EQ(t->_raw.width, c.side) << c.format;
        if (c.s == planes) {
            EXPECT_FALSE(t->_raw.blockLinear) << "a planar surface's planes stay rows";
            EXPECT_EQ(t->_raw.pitch, uint32_t(IOSurfaceGetBytesPerRowOfPlane(c.s, c.plane))) << c.format;
        }
    }
    EXPECT_EQ([device newTextureWithDescriptor:turned(130, 1, kSide, kSide) iosurface:words plane:0], nil);
    for (IOSurfaceRef s : {two, words, half, planes})
        CFRelease(s);
}

// A turned RGB10A8_2P_XR10 over two planes or over one, its sRGB twin, and a turned YCbCr texture are made at their
// plane's extent (an M4 Max makes those over two planes and the YCbCr one: m2v-textures4 probes/forms.mm): held in the
// planes, or a YCbCr image whose stand-in and store states are laid out as the planes
void planarFormatsTurn()
{
    auto states = conversions();
    states.insert(states.end(), {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                 {"raw.ycbcr_store.ycbcr_store_luma8_frag", MTLPixelFormatR8Unorm},
                                 {"raw.ycbcr_store.ycbcr_store_chroma8_frag", MTLPixelFormatRG8Unorm}});
    kernel_model::model.infoFlags = TegraGM20BInfoSparse;  // a plane alone's alpha is a sparse range
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", states));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef planes = twoPlanes(), words = chunky('w30r', 4), video = fourTwoZero();
    ASSERT_TRUE(planes && words && video);

    const struct {
        IOSurfaceRef s;
        uint32_t format;
        NSUInteger rotation;
    } kTurned[] = {{planes, 550, 1}, {planes, 551, 3}, {words, 550, 2}, {words, 551, 4}, {video, 500, 1}};

    for (const auto &c : kTurned) {
        SwitchMetalTexture *t =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:turned(c.format, c.rotation, kSide / 2, kSide)
                                                         iosurface:c.s
                                                             plane:0];
        ASSERT_TRUE(t != nil && t->_gm20bOk) << c.format << " rotation " << c.rotation;
        const gm20b::Texture &x = t->_raw;
        const bool half = c.rotation == 2;
        const NSUInteger planeWidth = half ? kSide / 2 : kSide, planeHeight = half ? kSide : kSide / 2;
        EXPECT_EQ(x.rotation, c.rotation) << c.format;
        EXPECT_EQ(x.width, planeWidth) << c.format;
        EXPECT_EQ(x.height, planeHeight) << c.format;
        if (c.format == 500) {
            ASSERT_TRUE(x.ycbcr && x.planarStandIn && x.ycbcrStore);
            EXPECT_EQ(x.planarStandIn->width, planeWidth);
            continue;
        }
        ASSERT_TRUE(x.xrPlanes && x.luma && x.chroma) << c.format;
        EXPECT_EQ(x.luma->width, planeWidth) << c.format;
        EXPECT_EQ(x.chroma->memoryless, c.s == words) << c.format;
    }
    for (IOSurfaceRef s : {planes, words, video})
        CFRelease(s);
}

IOSurfaceRef row(NSUInteger texels, NSUInteger bytes)
{
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(texels),
        (id)kIOSurfaceHeight : @1,
        (id)kIOSurfaceBytesPerElement : @(bytes),
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
}

// An extended-range texel buffer over a surface reads and stores as one over a buffer of the same bytes, through the same
// stand-in; RGB10A8_2P_XR10's and its sRGB twin's read BGR10_XR's words, alpha 0, and one written holds the same words,
// which an M4 Max's kernels and fragment functions store alone, bits 30 and 31 clear
void extendedRangeTexelBuffersOverASurface()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", conversions()));
    ASSERT_TRUE(device != nil);
    const auto buffer = [](uint32_t format, MTLTextureUsage usage) {
        return [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                      width:1024
                                                            resourceOptions:0
                                                                      usage:usage];
    };
    for (uint32_t format = 550; format <= 555; format++) {
        IOSurfaceRef s = row(4096, format == 552 || format == 553 ? 8 : 4);
        ASSERT_TRUE(s);
        SwitchMetalTexture *t =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:buffer(format, MTLTextureUsageShaderRead)
                                                         iosurface:s
                                                             plane:0];
        ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.standIn) << format;
        EXPECT_EQ(t->_raw.type, gm20b::fx::kTexOneDBuffer) << format;
        const bool words = format == 550 || format == 551;
        EXPECT_EQ(t->_raw.format, words ? format + 4 : format) << format;
        if (words) {
            EXPECT_EQ(t->_raw.standIn->swizzle[3], uint8_t(MTLTextureSwizzleZero)) << "alpha 0";
            SwitchMetalTexture *w =
                (SwitchMetalTexture *)[device newTextureWithDescriptor:buffer(format, MTLTextureUsageShaderWrite)
                                                             iosurface:s
                                                                 plane:0];
            ASSERT_TRUE(w != nil && w->_gm20bOk) << format;
            EXPECT_EQ(w->_raw.format, format + 4) << format;
        }
        CFRelease(s);
    }
}

// A view of RGB10A8_2P_XR10 held as BGR10_XR's words reads them as Metal's does: as its sRGB twin through BGR10_XR_sRGB's
// header (an M4 Max: m2v-textures4 probes/reads.mm view), and in either format with alpha 0; BGR10_XR is no view of it
void wordsViewsReadAlphaZero()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", conversions()));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = chunky('w30r', 4), planes = twoPlanes();
    ASSERT_TRUE(s && planes);
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device
        newTextureWithDescriptor:flat(550, MTLTextureUsage(MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView))
                       iosurface:s
                           plane:0];
    ASSERT_TRUE(t != nil && t->_gm20bOk);
    for (const uint32_t format : {551u, 550u}) {
        SwitchMetalTexture *v =
            (SwitchMetalTexture *)[(id<MTLTexture>)t newTextureViewWithPixelFormat:MTLPixelFormat(format)];
        ASSERT_TRUE(v != nil && v->_gm20bOk && v->_raw.standIn) << format;
        EXPECT_EQ(v->_raw.format, format + 4) << format;
        EXPECT_EQ(v->_raw.standIn->swizzle[3], uint8_t(MTLTextureSwizzleZero)) << format;
    }
    EXPECT_EQ([(id<MTLTexture>)t newTextureViewWithPixelFormat:MTLPixelFormat(554)], nil);
    // over a later plane of several: that plane's bytes as the words, at its rows
    SwitchMetalTexture *later =
        (SwitchMetalTexture *)[device newTextureWithDescriptor:flat(550, MTLTextureUsageShaderRead, kSide / 4)
                                                     iosurface:planes
                                                         plane:1];
    ASSERT_TRUE(later != nil && later->_gm20bOk);
    EXPECT_EQ(later->_raw.format, 554u);
    EXPECT_EQ(later->_raw.pitch, uint32_t(kSide));
    for (IOSurfaceRef x : {s, planes})
        CFRelease(x);
}

// MTLPixelFormatUnspecialized: no texture by descriptor, view or buffer (an M4 Max; both identities answer it as Invalid)
void unspecializedIsNoTexturesFormat()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    constexpr MTLPixelFormat kUnspecialized = MTLPixelFormat(263);
    EXPECT_EQ([device newTextureWithDescriptor:flat(263, MTLTextureUsageShaderRead)], nil);
    id<MTLTexture> words = [device newTextureWithDescriptor:flat(MTLPixelFormatR32Uint, MTLTextureUsageShaderRead)];
    ASSERT_TRUE(words != nil);
    EXPECT_EQ([words newTextureViewWithPixelFormat:kUnspecialized], nil);
    id<MTLBuffer> b = [device newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared];
    EXPECT_EQ([b newTextureWithDescriptor:flat(263, MTLTextureUsageShaderRead) offset:0 bytesPerRow:kSide * 4], nil);
}

// X32_Stencil8 in a heap: a Depth32Float_Stencil8 texture's memory read through its stencil, its size and alignment that
// texture's
void aStencilViewFormatInAHeapIsADepthStencilsMemory()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *stencil = flat(MTLPixelFormatX32_Stencil8, MTLTextureUsageShaderRead),
                         *combined = flat(MTLPixelFormatDepth32Float_Stencil8, MTLTextureUsageShaderRead);
    stencil.storageMode = combined.storageMode = MTLStorageModePrivate;
    const MTLSizeAndAlign x = [device heapTextureSizeAndAlignWithDescriptor:stencil],
                          z = [device heapTextureSizeAndAlignWithDescriptor:combined];
    ASSERT_GT(z.size, 0u);
    EXPECT_EQ(x.size, z.size);
    EXPECT_EQ(x.align, z.align);
    MTLHeapDescriptor *h = [MTLHeapDescriptor new];
    h.storageMode = MTLStorageModePrivate;
    h.size = 4 * z.size;
    id<MTLHeap> heap = [device newHeapWithDescriptor:h];
    ASSERT_TRUE(heap != nil);
    SwitchMetalTexture *t = (SwitchMetalTexture *)[heap newTextureWithDescriptor:stencil];
    ASSERT_TRUE(t != nil && t->_gm20bOk);
    EXPECT_EQ([(id<MTLTexture>)t pixelFormat], MTLPixelFormatX32_Stencil8);
    EXPECT_TRUE(t->_raw.zeta) << "the combined texture's stencil";
}

// A b3a8-like surface of a `wordBytes` and an `alphaBytes` plane, rows of their elements
IOSurfaceRef twoPlanesOf(unsigned wordBytes, unsigned alphaBytes)
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned plane = 0; plane < 2; plane++) {
        const unsigned bytes = plane ? alphaBytes : wordBytes;
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(kSide),
            (id)kIOSurfacePlaneHeight : @(kSide),
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(kSide * bytes),
            (id)kIOSurfacePlaneOffset : @(plane ? kSide * kSide * wordBytes : 0),
            (id)kIOSurfacePlaneSize : @(kSide * kSide * bytes)
        }];
    }
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(kSide * kSide * (wordBytes + alphaBytes)),
        (id)kIOSurfacePlaneInfo : planes
    });
}

// An M4 Max reads RGB10A8_2P_XR10 over plane 0 of any two-plane surface by the two planes' rows, whatever their elements
// (m2v-textures5 probes/xrplane0.mm): held in its planes, rows of fewer bytes than its words read overlapping, at their
// pitch, through pools
void extendedRangeOverTwoPlanesOfAnyElements()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", conversions()));
    ASSERT_TRUE(device != nil);

    const struct {
        unsigned word, alpha;
    } kPlanes[] = {{4, 1}, {4, 2}, {8, 1}, {4, 4}, {2, 1}};

    for (const auto &c : kPlanes) {
        IOSurfaceRef s = twoPlanesOf(c.word, c.alpha);
        ASSERT_TRUE(s);
        SwitchMetalTexture *t =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:flat(550, MTLTextureUsageShaderRead)
                                                         iosurface:s
                                                             plane:0];
        CFRelease(s);
        ASSERT_TRUE(t != nil && t->_gm20bOk) << c.word << "+" << c.alpha;
        EXPECT_TRUE(t->_raw.xrPlanes) << c.word << "+" << c.alpha;
    }
}

// hw_test_app's gpu.gm20b.texel_buffer_reach (debug builds): a texel buffer from a buffer's first byte of the count asked, nil past
// 32 bits or the buffer. The count past the device's maxTextureBufferWidth answer of 2^27 is that HW Test's to show: here it
// would take a buffer of 128 MiB
void theTexelBufferReachEntryPoint()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    constexpr NSUInteger kBytes = 1 << 16;
    id<MTLBuffer> b = [device newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(b != nil);
    const auto reach = [&](NSUInteger n) {
        return ((id (*)(id, SEL, id, MTLPixelFormat, NSUInteger))objc_msgSend)(
            device, sel_registerName("switchMetalTexelBuffer:format:width:"), b, MTLPixelFormatR8Unorm, n);
    };
    SwitchMetalTexture *t = (SwitchMetalTexture *)reach(kBytes);
    ASSERT_TRUE(t != nil && t->_gm20bOk);
    EXPECT_EQ(t->_raw.type, gm20b::fx::kTexOneDBuffer);
    EXPECT_EQ(t->_raw.width, uint32_t(kBytes));
    EXPECT_EQ(reach(kBytes + 1), nil) << "past its buffer";
    EXPECT_EQ(reach(NSUInteger(UINT32_MAX) + 1), nil);
}

}  // namespace

TEST(GM20BFacadeSurfaceShapes, AStencilViewFormatInAHeapIsADepthStencilsMemory)
{
    switch_ios::test::ExpectPassesInFreshProcess(aStencilViewFormatInAHeapIsADepthStencilsMemory);
}

TEST(GM20BFacadeSurfaceShapes, ExtendedRangeOverTwoPlanesOfAnyElementsIsHeldInThem)
{
    switch_ios::test::ExpectPassesInFreshProcess(extendedRangeOverTwoPlanesOfAnyElements);
}

TEST(GM20BFacadeSurfaceShapes, TheTexelBufferReachEntryPoint)
{
    switch_ios::test::ExpectPassesInFreshProcess(theTexelBufferReachEntryPoint);
}

TEST(GM20BFacadeSurfaceShapes, AReadOnlyYcbcrTextureIsATarget)
{
    switch_ios::test::ExpectPassesInFreshProcess(aReadOnlyYcbcrTextureIsATarget);
}

TEST(GM20BFacadeSurfaceShapes, EveryRenderedFormatTurns)
{
    switch_ios::test::ExpectPassesInFreshProcess(everyRenderedFormatTurns);
}

TEST(GM20BFacadeSurfaceShapes, PlanarFormatsTurnAtTheirPlanesExtent)
{
    switch_ios::test::ExpectPassesInFreshProcess(planarFormatsTurn);
}

TEST(GM20BFacadeSurfaceShapes, ExtendedRangeTexelBuffersOverASurfaceAreRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(extendedRangeTexelBuffersOverASurface);
}

TEST(GM20BFacadeSurfaceShapes, ViewsOfExtendedRangeWordsReadAlphaZero)
{
    switch_ios::test::ExpectPassesInFreshProcess(wordsViewsReadAlphaZero);
}

TEST(GM20BFacadeSurfaceShapes, UnspecializedIsNoTexturesFormat)
{
    switch_ios::test::ExpectPassesInFreshProcess(unspecializedIsNoTexturesFormat);
}

TEST(GM20BFacadeSurfaceShapes, ExtendedRangeTexelBuffersAreRawsWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(extendedRangeTexelBuffers);
}

TEST(GM20BFacadeSurfaceShapes, EverySurfaceShapeEndsOnRawWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(surfaceShapesEndOnRaw);
}
