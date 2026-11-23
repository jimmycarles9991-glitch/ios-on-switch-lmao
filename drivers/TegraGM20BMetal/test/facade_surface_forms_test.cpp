// SPDX-License-Identifier: MIT
// Textures over surfaces the driver once answered nil for, as Metal makes them: each expectation is an M4 Max's
// (m2v-textures9 probes/xr.txt, ycbcr.txt).
#include "facade_pass_scene.h"
#include "../attachment_target.h"
#import "../TegraGM20BMetalXrPlanes.h"

namespace {

using namespace pass_scene;

struct PlaneShape {
    unsigned bytes, rows;
};

// A 'b3a8' surface of 64x64 whose planes lie one after another, each `rows` bytes a row
IOSurfaceRef planesSurface(std::initializer_list<PlaneShape> shapes)
{
    NSMutableArray *planes = [NSMutableArray array];
    unsigned offset = 0;
    for (const PlaneShape &p : shapes) {
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @64,
            (id)kIOSurfacePlaneHeight : @64,
            (id)kIOSurfacePlaneBytesPerElement : @(p.bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(p.rows),
            (id)kIOSurfacePlaneOffset : @(offset),
            (id)kIOSurfacePlaneSize : @(64 * p.rows)
        }];
        offset += 64 * p.rows;
    }
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(offset),
        (id)kIOSurfacePlaneInfo : planes
    });
}

MTLTextureDescriptor *sampled(uint32_t format, NSUInteger side)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                 width:side
                                                                                height:side
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    return d;
}

const uint8_t *planeBytes(IOSurfaceRef s, size_t plane)
{
    return static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
}

// RGB10A8_2P_XR10 over a plane followed by another reads that plane's words and the next one's alpha bytes, over the last
// plane its words alone with alpha 0 (an M4 Max over planes 0, 1 and 2 of three: probes/xr.txt 1g, 1n, 1o)
void readsItsPlaneAndTheNext()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                                           {gm20b::xrConversionOf(554), MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = planesSurface({{4, 256}, {4, 256}, {4, 256}});
    ASSERT_TRUE(s);
    for (size_t plane = 0; plane < 3; plane++) {
        SwitchMetalTexture *t =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:sampled(550, 64) iosurface:s plane:plane];
        ASSERT_TRUE(t != nil && t->_gm20bOk) << "plane " << plane;
        const gm20b::Texture &x = t->_raw;
        if (plane == 2) {
            EXPECT_FALSE(x.xrPlanes) << "the last plane: its words alone";
            EXPECT_EQ(x.cpu, planeBytes(s, 2));
            continue;
        }
        ASSERT_TRUE(x.xrPlanes && x.luma && x.chroma) << "plane " << plane;
        EXPECT_EQ(x.luma->cpu, planeBytes(s, plane)) << "its words";
        EXPECT_EQ(x.chroma->cpu, planeBytes(s, plane + 1)) << "the next plane's alpha bytes";
        EXPECT_FALSE(x.chroma->memoryless) << plane;
    }
    CFRelease(s);
}

// YCBCR8_420_2P reads luma from its plane and chroma from the next one where there is one, chroma codes 0 where there is
// none: over planes 1 and 2 of three, and over a surface of one plane (an M4 Max: probes/ycbcr.txt 3aj, 3s). A texture of no
// width is Metal's descriptor validation's stop (probes/ycbcr.txt, empty).
void ycbcrReadsItsPlaneAndTheNext()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef three = planesSurface({{1, 64}, {1, 64}, {2, 128}});
    IOSurfaceRef one = planesSurface({{4, 256}});
    ASSERT_TRUE(three && one);
    SwitchMetalTexture *t =
        (SwitchMetalTexture *)[device newTextureWithDescriptor:sampled(500, 32) iosurface:three plane:1];
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "over plane 1 of three";
    ASSERT_TRUE(t->_raw.luma && t->_raw.chroma);
    EXPECT_EQ(t->_raw.luma->cpu, planeBytes(three, 1));
    EXPECT_EQ(t->_raw.chroma->cpu, planeBytes(three, 2));
    EXPECT_EQ(t->_raw.shape[14], 1u);
    t = (SwitchMetalTexture *)[device newTextureWithDescriptor:sampled(500, 64) iosurface:one plane:0];
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "over a surface of one plane";
    ASSERT_TRUE(t->_raw.luma);
    EXPECT_EQ(t->_raw.luma->cpu, planeBytes(one, 0));
    EXPECT_EQ(t->_raw.chroma, nullptr);
    EXPECT_EQ(t->_raw.shape[14], 0u) << "chroma codes 0";
    MTLTextureDescriptor *empty = sampled(500, 64);
    empty.width = 0;
    EXPECT_EQ([device newTextureWithDescriptor:empty iosurface:one plane:0], nil);
    CFRelease(three);
    CFRelease(one);
}

// RGB10A8_2P_XR10 over planes whose offsets or rows are off 16: Metal's GPU reads, writes and blits them at both rounded down
// to 16, and its CPU copies take the word plane's own rows (an M4 Max: probes/xr.txt 5a-5n)
void planesOffSixteenAreRoundedForTheGpu()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                                           {gm20b::xrConversionOf(554), MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef two = planesSurface({{4, 264}, {1, 72}});
    IOSurfaceRef one = planesSurface({{4, 264}});
    ASSERT_TRUE(two && one);
    const uint32_t word = 0x2abcdef1;
    for (IOSurfaceRef s : {two, one}) {
        SwitchMetalTexture *t =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:sampled(550, 64) iosurface:s plane:0];
        ASSERT_TRUE(t != nil && t->_gm20bOk) << IOSurfaceGetPlaneCount(s) << " planes";
        const gm20b::Texture &words = t->_raw.xrPlanes ? *t->_raw.luma : t->_raw;
        EXPECT_EQ(words.pitch, 256u) << "rows of 264 as 256";
        if (s == two) {
            ASSERT_TRUE(t->_raw.xrPlanes);
            EXPECT_EQ(t->_raw.chroma->pitch, 64u) << "rows of 72 as 64";
        }
        EXPECT_EQ(t->_cpuRows, planeBytes(s, 0));
        EXPECT_EQ(t->_cpuPitch, 264u);
        [(id<MTLTexture>)t replaceRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0 withBytes:&word bytesPerRow:4];
        EXPECT_EQ(memcmp(planeBytes(s, 0) + 3 * 264 + 5 * 4, &word, 4), 0) << "the CPU writes the plane's own rows";
        uint32_t back = 0;
        [(id<MTLTexture>)t getBytes:&back bytesPerRow:4 fromRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0];
        EXPECT_EQ(back, word);
    }
    CFRelease(two);
    CFRelease(one);
}

// An 8x8 '420v' of 8 MiB + 16 KiB, plane 0 bytes `rows` apart at `luma`, plane 1 pairs `rows` apart at `chroma`
IOSurfaceRef narrowPlanes(unsigned rows, unsigned luma, unsigned chroma)
{
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @8,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('420v')),
        (id)kIOSurfaceAllocSize : @((8u << 20) + 16384),
        (id)kIOSurfacePlaneInfo : @[
            @{
                (id)kIOSurfacePlaneWidth : @8,
                (id)kIOSurfacePlaneHeight : @8,
                (id)kIOSurfacePlaneBytesPerElement : @1,
                (id)kIOSurfacePlaneBytesPerRow : @(rows),
                (id)kIOSurfacePlaneOffset : @(luma),
                (id)kIOSurfacePlaneSize : @(8 * rows)
            },
            @{
                (id)kIOSurfacePlaneWidth : @4,
                (id)kIOSurfacePlaneHeight : @4,
                (id)kIOSurfacePlaneBytesPerElement : @2,
                (id)kIOSurfacePlaneBytesPerRow : @(rows),
                (id)kIOSurfacePlaneOffset : @(chroma),
                (id)kIOSurfacePlaneSize : @(4 * rows)
            }
        ]
    });
}

// A two-plane YCbCr texture's CPU copies move its luma codes alone, a code a texel at its plane's own offset and rows
// (8 at 8, which its GPU reads 1 MiB apart from 0), never its chroma; a packed 10-bit one's move nothing (an M4 Max:
// m2v-textures13 probes/c4misc/RESULT.txt P1, P2)
void ycbcrCpuCopiesMoveTheLumaPlaneAlone()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.ycbcr.ycbcr_packed_converted_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef narrow = narrowPlanes(8, 8, 4100), wide = narrowPlanes(16, 0, 4096);
    ASSERT_TRUE(narrow && wide);
    uint8_t *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(narrow));
    memset(base, 0xee, 8192);
    SwitchMetalTexture *t =
        (SwitchMetalTexture *)[device newTextureWithDescriptor:sampled(500, 8) iosurface:narrow plane:0];
    ASSERT_TRUE(t != nil && t->_gm20bOk);
    uint8_t codes[4 * 16];
    for (unsigned i = 0; i < sizeof codes; i++)
        codes[i] = uint8_t(i * 7 + 3);
    [(id<MTLTexture>)t replaceRegion:MTLRegionMake2D(2, 3, 4, 4) mipmapLevel:0 withBytes:codes bytesPerRow:16];
    for (unsigned a = 0; a < 8192; a++) {
        const unsigned y = (a - 8) / 8, x = (a - 8) % 8;
        const bool region = a >= 8 && y >= 3 && y < 7 && x >= 2 && x < 6;
        EXPECT_EQ(base[a], region ? codes[(y - 3) * 16 + x - 2] : 0xee) << "byte " << a;
    }
    uint8_t back[4 * 16];
    memset(back, 0x55, sizeof back);
    [(id<MTLTexture>)t getBytes:back bytesPerRow:16 fromRegion:MTLRegionMake2D(2, 3, 4, 4) mipmapLevel:0];
    for (unsigned i = 0; i < sizeof back; i++)
        EXPECT_EQ(back[i], i % 16 < 4 ? codes[i] : 0x55) << "buffer byte " << i;
    SwitchMetalTexture *p =
        (SwitchMetalTexture *)[device newTextureWithDescriptor:sampled(508, 8) iosurface:wide plane:0];
    ASSERT_TRUE(p != nil && p->_gm20bOk);
    uint8_t *bytes = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(wide));
    memset(bytes, 0xee, 8192);
    [(id<MTLTexture>)p replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:codes bytesPerRow:16];
    EXPECT_EQ(std::count(bytes, bytes + 8192, 0xee), 8192) << "a packed 10-bit texture's replaceRegion moves nothing";
    memset(back, 0x55, sizeof back);
    [(id<MTLTexture>)p getBytes:back bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    EXPECT_EQ(std::count(back, back + sizeof back, 0x55), long(sizeof back)) << "nor its getBytes";
    CFRelease(narrow);
    CFRelease(wide);
}

// A texture buffer of a YCbCr or packed 10-bit plane format over a surface's plane, which an M4 Max makes (validation on
// and off) and reads as a 2D texture of the format's row 0 (64 of 64 texels: m2v-textures13 probes/ytbuf/ytbuf.txt,
// ytread.txt): its converted image one linear row under a texel buffer's header
void ycbcrTexelBuffersAreTheirFirstRow()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.ycbcr.ycbcr_codes_frag", MTLPixelFormatRGBA8Unorm},
                                           {"raw.ycbcr.ycbcr_packed_plane_frag", MTLPixelFormatRGB10A2Unorm}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = narrowPlanes(64, 0, 4096);
    ASSERT_TRUE(s);
    for (const uint32_t format : {500u, 540u, 588u}) {
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                   width:8
                                                         resourceOptions:MTLResourceStorageModeShared
                                                                   usage:MTLTextureUsageShaderRead];
        SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
        ASSERT_TRUE(t != nil && t->_gm20bOk) << format << ": Metal makes it";
        const gm20b::Texture &x = t->_raw;
        EXPECT_TRUE(x.converted()) << format;
        EXPECT_EQ(x.type, gm20b::fx::kTexOneDBuffer) << format;
        EXPECT_FALSE(x.blockLinear) << format << ": a texel buffer's header reads linear memory";
        EXPECT_EQ(x.width, 8u) << format;
        EXPECT_EQ(x.height, 1u) << format;
        EXPECT_EQ(gRM->raw->headerWords(x.tic)[2] >> 21 & 7, 0u) << format << ": HEADER_VERSION SELECT_ONE_D_BUFFER";
    }
    CFRelease(s);
}

// A YCbCr or packed 10-bit texture buffer wider than 16384 texels, which an M4 Max makes and reads texel i of at its plane's
// (i % 16384, i / 16384) (20000 texels, every format: HW Test gpu.gm20b.ycbcr_texel_buffer_wide's scene): planned as the 2D
// texture of rows 16384 wide, its image those rows back to back under the buffer's header of 20000 texels, which its
// conversion draws as one target of its rows
void wideYcbcrTexelBuffersAreRowsOf16384()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.ycbcr.ycbcr_codes_frag", MTLPixelFormatRGBA8Unorm},
                                           {"raw.ycbcr.ycbcr_packed_plane_frag", MTLPixelFormatRGB10A2Unorm}}));
    ASSERT_TRUE(device != nil);
    constexpr unsigned kWidth = 20000, kRows = 26880;  // a row of 20000 bytes or 6667 packed words, rounded to 256
    for (const uint32_t format : {500u, 540u, 588u}) {
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef) @{
            (id)kIOSurfaceWidth : @(kWidth),
            (id)kIOSurfaceHeight : @2,
            (id)kIOSurfacePixelFormat : @(format == 588 ? uint32_t('p420') : uint32_t('420v')),
            (id)kIOSurfaceAllocSize : @(4 * kRows + 16384),
            (id)kIOSurfacePlaneInfo : @[
                @{
                    (id)kIOSurfacePlaneWidth : @(kWidth),
                    (id)kIOSurfacePlaneHeight : @2,
                    (id)kIOSurfacePlaneBytesPerElement : @1,
                    (id)kIOSurfacePlaneBytesPerRow : @(kRows),
                    (id)kIOSurfacePlaneOffset : @0,
                    (id)kIOSurfacePlaneSize : @(2 * kRows)
                },
                @{
                    (id)kIOSurfacePlaneWidth : @(kWidth / 2),
                    (id)kIOSurfacePlaneHeight : @1,
                    (id)kIOSurfacePlaneBytesPerElement : @2,
                    (id)kIOSurfacePlaneBytesPerRow : @(kRows),
                    (id)kIOSurfacePlaneOffset : @(2 * kRows),
                    (id)kIOSurfacePlaneSize : @(kRows)
                }
            ]
        });
        ASSERT_TRUE(s);
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                   width:kWidth
                                                         resourceOptions:MTLResourceStorageModeShared
                                                                   usage:MTLTextureUsageShaderRead];
        SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
        CFRelease(s);
        ASSERT_TRUE(t != nil && t->_gm20bOk) << format << ": Metal makes it";
        const gm20b::Texture &x = t->_raw;
        EXPECT_EQ(x.type, gm20b::fx::kTexOneDBuffer) << format;
        EXPECT_EQ(x.width, kWidth) << format;
        EXPECT_EQ(x.shape[0], 16384u) << format << ": converted as the 2D texture of rows 16384 wide";
        EXPECT_EQ(x.shape[1], 2u) << format;
        EXPECT_EQ(x.luma->height, 2u) << format << ": the plane's rows 0 and 1";
        const uint32_t *w = gRM->raw->headerWords(x.tic);
        EXPECT_EQ((w[3] & 0xffff) << 16 | (w[4] & 0xffff), kWidth - 1) << format << ": the header's texels";
        gm20b::Texture rows;
        ASSERT_TRUE(gm20b::texelBufferRows(x, rows)) << format;
        EXPECT_EQ(rows.width, 16384u) << format;
        EXPECT_EQ(rows.height, 2u) << format;
        EXPECT_EQ(rows.pitch, 16384u * rows.bpp) << format << ": rows back to back";
    }
}

}  // namespace

TEST(GM20BFacadeSurfaceForms, RGB10A8XrReadsItsPlaneAndTheNext)
{
    switch_ios::test::ExpectPassesInFreshProcess(readsItsPlaneAndTheNext);
}

TEST(GM20BFacadeSurfaceForms, YcbcrReadsItsPlaneAndTheNext)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrReadsItsPlaneAndTheNext);
}

TEST(GM20BFacadeSurfaceForms, RGB10A8XrPlanesOffSixteenAreRoundedForTheGpu)
{
    switch_ios::test::ExpectPassesInFreshProcess(planesOffSixteenAreRoundedForTheGpu);
}

TEST(GM20BFacadeSurfaceForms, YcbcrCpuCopiesMoveTheLumaPlaneAlone)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrCpuCopiesMoveTheLumaPlaneAlone);
}

TEST(GM20BFacadeSurfaceForms, YcbcrTexelBuffersAreTheirFirstRow)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrTexelBuffersAreTheirFirstRow);
}

TEST(GM20BFacadeSurfaceForms, WideYcbcrTexelBuffersAreRowsOf16384)
{
    switch_ios::test::ExpectPassesInFreshProcess(wideYcbcrTexelBuffersAreRowsOf16384);
}
