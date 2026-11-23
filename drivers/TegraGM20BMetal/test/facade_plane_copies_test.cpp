// SPDX-License-Identifier: MIT
// Blits of textures held through a surface's planes as the facade records them (facade_harness.h), run on the
// copy engine's model (facade_blit_engines.h), against an M4 Max's bytes under macOS 27.2.1 with validation off
// (blit_planes.h): plane 0 counted in a wider texel runs on as one run of bytes, whatever its rows' padding, up to
// its surface's last page; between two such textures plane 1 moves a byte a texel at plane 0's rows; a one-plane 4:4:4
// texture (504, 524) counts a word a texel and has no plane 1; a colour fill of RGB10A8_2P_XR10 planes is undefined
// there and writes nothing
#include "facade_blit_engines.h"
#include "../TegraGM20BMetalInternal.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <random>
#include <string>
#include <vector>

@interface NSObject (SwitchPlaneCopiesTest)
- (void)fillTexture:(id<MTLTexture>)texture
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              color:(MTLClearColor)color;
@end

namespace {

using namespace harness;

constexpr NSUInteger kWidth = 16, kHeight = 8;

id<MTLDevice> gDevice;

class FacadePlaneCopies : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                 {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                  {"raw.ycbcr.ycbcr_srgb_frag", MTLPixelFormatRGBA32Float}}));
        ASSERT_TRUE(gDevice != nil);
    }
};

// 'b3a8' of rows `pitch` bytes apart, its alpha plane's 64 (a two-plane XR plane's rows are 32-byte multiples)
IOSurfaceRef xrSurface(NSUInteger pitch)
{
    NSDictionary *words = @{
        (id)kIOSurfacePlaneWidth : @(kWidth),
        (id)kIOSurfacePlaneHeight : @(kHeight),
        (id)kIOSurfacePlaneBytesPerElement : @4,
        (id)kIOSurfacePlaneBytesPerRow : @(pitch),
        (id)kIOSurfacePlaneOffset : @0,
        (id)kIOSurfacePlaneSize : @(pitch * kHeight)
    };
    NSDictionary *alphas = @{
        (id)kIOSurfacePlaneWidth : @(kWidth),
        (id)kIOSurfacePlaneHeight : @(kHeight),
        (id)kIOSurfacePlaneBytesPerElement : @1,
        (id)kIOSurfacePlaneBytesPerRow : @64,
        (id)kIOSurfacePlaneOffset : @(pitch * kHeight),
        (id)kIOSurfacePlaneSize : @(64 * kHeight)
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kWidth),
        (id)kIOSurfaceHeight : @(kHeight),
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @((pitch + 64) * kHeight),
        (id)kIOSurfacePlaneInfo : @[ words, alphas ]
    });
}

// A surface of one plane, `pairs` ('2vuy': two pixels a four-byte element) or a word a pixel ('v410'), rows exactly as
// long as its pixels
IOSurfaceRef onePlaneSurface(uint32_t fourcc, NSUInteger rows)
{
    const bool pairs = fourcc == uint32_t('2vuy');
    const NSUInteger pitch = kWidth * (pairs ? 2 : 4);
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kWidth),
        (id)kIOSurfaceHeight : @(rows),
        (id)kIOSurfacePixelFormat : @(fourcc),
        (id)kIOSurfaceBytesPerRow : @(pitch),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceElementWidth : @(pairs ? 2 : 1),
        (id)kIOSurfaceAllocSize : @(pitch * rows)
    });
}

id<MTLTexture> over(IOSurfaceRef s, uint32_t format)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                 width:kWidth
                                                                                height:IOSurfaceGetHeight(s)
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [gDevice newTextureWithDescriptor:d iosurface:s plane:0];
    EXPECT_TRUE([(id)t isKindOfClass:[SwitchMetalTexture class]] && ((SwitchMetalTexture *)t)->_gm20bOk)
        << format << " not the driver's";
    return t;
}

// Every byte of the surface at random; those bytes
std::vector<uint8_t> seeded(IOSurfaceRef s, uint32_t seed)
{
    std::mt19937 rng(seed);
    auto *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(s));
    for (size_t i = 0; i < IOSurfaceGetAllocSize(s); ++i)
        base[i] = uint8_t(rng());
    return std::vector<uint8_t>(base, base + IOSurfaceGetAllocSize(s));
}

std::vector<uint8_t> bytesOf(IOSurfaceRef s)
{
    auto *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(s));
    return std::vector<uint8_t>(base, base + IOSurfaceGetAllocSize(s));
}

// A 16x8 texture of `format` (`bytes` a texel) over a buffer of 0xee, its rows the buffer's
id<MTLTexture> plainOver(id<MTLBuffer> rows, MTLPixelFormat format, NSUInteger bytes)
{
    memset(rows.contents, 0xee, rows.length);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:kWidth height:kHeight mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [rows newTextureWithDescriptor:d offset:0 bytesPerRow:kWidth * bytes];
    EXPECT_TRUE([(id)t isKindOfClass:[SwitchMetalTexture class]] && ((SwitchMetalTexture *)t)->_gm20bOk);
    return t;
}

// `size` texels from `from` of one texture to `to` of another in a command buffer of its own; whether the copy was
// recorded, its command buffer then committed on the driver and its engines' work run
bool copied(id<MTLTexture> a, MTLOrigin from, MTLSize size, id<MTLTexture> b, MTLOrigin to)
{
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    [e copyFromTexture:a
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:from
               sourceSize:size
                toTexture:b
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:to];
    const bool taken = ((SwitchMetalCommandBuffer *)cb)->_plan.size() != recorded;
    [e endEncoding];
    EngineRun run;
    commitOnRaw(cb, run);
    return taken;
}

// Each plain byte against plane 0 read as one run from (x0, y0) in the plain texture's texel of `bytes`:
// plain(B + i, 1 + k) = p0(x0 * B + i, y0 + k) for i < 8B, k < 4, every other byte as it was (0xee)
void expectOneRun(const std::vector<uint8_t> &before, NSUInteger pitch, NSUInteger x0, NSUInteger y0,
                  id<MTLBuffer> rows, NSUInteger bytes, const std::string &what)
{
    const auto *got = static_cast<const uint8_t *>(rows.contents);
    const NSUInteger row = kWidth * bytes;
    size_t wrong = 0;
    for (NSUInteger y = 0; y < kHeight; ++y)
        for (NSUInteger x = 0; x < row; ++x) {
            const bool in = y >= 1 && y < 5 && x >= bytes && x < 9 * bytes;
            const uint8_t want = in ? before[(y0 + y - 1) * pitch + x0 * bytes + (x - bytes)] : 0xee;
            wrong += got[y * row + x] != want;
        }
    EXPECT_EQ(wrong, 0u) << what;
}

// A one-page surface of two planes: plane 0 `rows0` bytes a row at 0 (`unit0` bytes a texel), plane 1 at `off1`,
// `rows1` bytes a row of `w1` x `h1` elements of `unit1` bytes
struct TwoPlanes {
    uint32_t fourcc, unit0, rows0, off1, unit1, w1, h1, rows1;

    IOSurfaceRef make() const
    {
        NSDictionary *zero = @{
            (id)kIOSurfacePlaneWidth : @(kWidth),
            (id)kIOSurfacePlaneHeight : @(kHeight),
            (id)kIOSurfacePlaneBytesPerElement : @(unit0),
            (id)kIOSurfacePlaneBytesPerRow : @(rows0),
            (id)kIOSurfacePlaneOffset : @0,
            (id)kIOSurfacePlaneSize : @(rows0 * kHeight)
        };
        NSDictionary *one = @{
            (id)kIOSurfacePlaneWidth : @(w1),
            (id)kIOSurfacePlaneHeight : @(h1),
            (id)kIOSurfacePlaneBytesPerElement : @(unit1),
            (id)kIOSurfacePlaneBytesPerRow : @(rows1),
            (id)kIOSurfacePlaneOffset : @(off1),
            (id)kIOSurfacePlaneSize : @(rows1 * h1)
        };
        return IOSurfaceCreate((__bridge CFDictionaryRef) @{
            (id)kIOSurfaceWidth : @(kWidth),
            (id)kIOSurfaceHeight : @(kHeight),
            (id)kIOSurfacePixelFormat : @(fourcc),
            (id)kIOSurfaceAllocSize : @16384,
            (id)kIOSurfacePlaneInfo : @[ zero, one ]
        });
    }
};

// The destination's bytes after a copy of `size` texels from `from` to `to` between textures over plane 0 of `s` and of
// `d`, as an M4 Max moves them (m2v-textures12 probes/f1.txt): plane 0 in the destination's unit at its rows; plane 1 a
// byte a texel at plane 0's rows, on through its end, source bytes past its page read as 0, destination bytes past its
// page dropped
std::vector<uint8_t> applesCopy(const TwoPlanes &s, const std::vector<uint8_t> &src, const TwoPlanes &d,
                                std::vector<uint8_t> dst, MTLOrigin from, MTLSize size, MTLOrigin to)
{
    for (NSUInteger y = 0; y < size.height; ++y) {
        for (NSUInteger b = 0; b < size.width * d.unit0; ++b)
            dst[(to.y + y) * d.rows0 + to.x * d.unit0 + b] = src[(from.y + y) * s.rows0 + from.x * d.unit0 + b];
        for (NSUInteger b = 0; b < size.width; ++b) {
            const size_t at = d.off1 + (to.y + y) * d.rows0 + to.x + b;
            const size_t of = s.off1 + (from.y + y) * s.rows0 + from.x + b;
            if (at < dst.size())
                dst[at] = of < src.size() ? src[of] : 0;
        }
    }
    return dst;
}

}  // namespace

// Planes into planes of their format: plane 1 a byte a texel of the region at each side's plane-0 rows, on through the
// plane's end into the bytes after it; its rows past the source's last page read as 0 and past the destination's are
// dropped (an M4 Max under macOS 27.2.1, validation off; probes/f1.txt A1-A8, Y1-Y2, A7)
TEST_F(FacadePlaneCopies, PlaneOneMovesAtPlaneZerosRowsOnThroughItsEnd)
{
    struct Case {
        uint32_t format;
        TwoPlanes s, d;
        MTLOrigin from;
        MTLSize size;
        MTLOrigin to;
    };

    const uint32_t b3a8 = 'b3a8', yuv = '420v';
    const TwoPlanes alpha32{b3a8, 4, 128, 1024, 1, kWidth, kHeight, 32};
    const TwoPlanes alpha48{b3a8, 4, 160, 2048, 1, kWidth, kHeight, 48};
    const TwoPlanes alphaAtEnd{b3a8, 4, 128, 16384 - 16 * kHeight, 1, kWidth, kHeight, 16};
    const TwoPlanes chroma32{yuv, 1, 48, 1024, 2, kWidth / 2, kHeight / 2, 32};
    const TwoPlanes chroma48{yuv, 1, 32, 512, 2, kWidth / 2, kHeight / 2, 48};
    for (const Case &c : {Case{550, alpha32, alpha48, {5, 3, 0}, {8, 4, 1}, {3, 1, 0}},
                          Case{550, alpha48, alpha32, {0, 0, 0}, {kWidth, kHeight, 1}, {0, 0, 0}},
                          Case{551, alpha32, alpha48, {2, 1, 0}, {12, 6, 1}, {1, 2, 0}},
                          Case{550, alpha32, alphaAtEnd, {0, 0, 0}, {kWidth, kHeight, 1}, {0, 0, 0}},
                          Case{550, alphaAtEnd, alpha32, {3, 0, 0}, {10, 8, 1}, {2, 0, 0}},
                          Case{500, chroma32, chroma48, {4, 2, 0}, {8, 4, 1}, {2, 4, 0}},
                          Case{500, chroma48, chroma32, {0, 0, 0}, {kWidth, kHeight, 1}, {0, 0, 0}}}) {
        const std::string what = std::to_string(c.format) + " rows " + std::to_string(c.s.rows0) + "/" +
                                 std::to_string(c.s.rows1) + " into " + std::to_string(c.d.rows0) + "/" +
                                 std::to_string(c.d.rows1) + ", plane 1 at " + std::to_string(c.d.off1);
        IOSurfaceRef s = c.s.make(), d = c.d.make();
        ASSERT_TRUE(s && d) << what;
        id<MTLTexture> a = over(s, c.format), b = over(d, c.format);
        const std::vector<uint8_t> src = seeded(s, c.format), before = seeded(d, c.format + 1);
        EXPECT_TRUE(copied(a, c.from, c.size, b, c.to)) << what;
        const std::vector<uint8_t> want = applesCopy(c.s, src, c.d, before, c.from, c.size, c.to), got = bytesOf(d);
        size_t wrong = 0, first = SIZE_MAX;
        for (size_t i = 0; i < got.size(); ++i)
            if (got[i] != want[i]) {
                ++wrong;
                first = std::min(first, i);
            }
        EXPECT_EQ(wrong, 0u) << what << ": first wrong byte " << first;
        EXPECT_EQ(bytesOf(s), src) << what << ": the source is only read";
        CFRelease(s);
        CFRelease(d);
    }
}

// A copy between textures of two pixel formats one of which is held through planes, which Metal's validation stops,
// moves what an M4 Max moves with validation off and is counted skipped: a plain source read in the planes' unit, each
// row's bytes in its own layout, plane 1 untouched (m2v-textures11 probes/h5/copies.txt, a texture over a buffer);
// between two two-plane formats both planes as between two of one format, plane 0 in the destination's unit
// (probes/h5x.txt)
TEST_F(FacadePlaneCopies, CopiesAcrossFormatsMoveTheBytesAppleMovesAndCountAsSkipped)
{
    const TwoPlanes alpha32{'b3a8', 4, 128, 1024, 1, kWidth, kHeight, 32};
    const TwoPlanes alpha48{'b3a8', 4, 160, 2048, 1, kWidth, kHeight, 48};
    const TwoPlanes chroma32{'420v', 1, 48, 1024, 2, kWidth / 2, kHeight / 2, 32};

    IOSurfaceRef d = alpha32.make();
    ASSERT_NE(d, nullptr);
    id<MTLTexture> planes = over(d, 550);
    id<MTLBuffer> rows = [gDevice newBufferWithLength:kWidth * kHeight options:MTLResourceStorageModeShared];
    id<MTLTexture> plain = plainOver(rows, MTLPixelFormatR8Unorm, 1);
    auto *bytes = static_cast<uint8_t *>(rows.contents);
    for (NSUInteger i = 0; i < rows.length; ++i)
        bytes[i] = uint8_t(i * 13 + 5);
    std::vector<uint8_t> want = seeded(d, 8);
    NSDictionary *before = harness::counters(gDevice);
    EXPECT_TRUE(copied(plain, MTLOriginMake(2, 1, 0), MTLSizeMake(4, 4, 1), planes, MTLOriginMake(3, 2, 0)));
    EXPECT_EQ(harness::skippedMoved(before, harness::counters(gDevice)), harness::skippedOnce("call.blit"));
    for (NSUInteger y = 0; y < 4; ++y)
        for (NSUInteger b = 0; b < 16; ++b)  // four texels of four bytes from byte 8 of each 16-byte row on
            want[(2 + y) * 128 + 12 + b] = bytes[(1 + y) * kWidth + 8 + b];
    EXPECT_EQ(bytesOf(d), want) << "R8Unorm rows read in four-byte texels; the alpha plane untouched";
    CFRelease(d);

    struct Case {
        uint32_t from, into;
        TwoPlanes s, d;
    };

    for (const Case &c :
         {Case{550, 551, alpha32, alpha48}, Case{551, 550, alpha48, alpha32}, Case{500, 550, chroma32, alpha48}}) {
        const std::string what = std::to_string(c.from) + " into " + std::to_string(c.into);
        IOSurfaceRef s = c.s.make(), t = c.d.make();
        ASSERT_TRUE(s && t) << what;
        id<MTLTexture> a = over(s, c.from), b = over(t, c.into);
        const std::vector<uint8_t> src = seeded(s, c.from), was = seeded(t, c.into);
        const MTLOrigin from = MTLOriginMake(2, 2, 0), to = MTLOriginMake(4, 2, 0);
        const MTLSize size = MTLSizeMake(4, 4, 1);
        NSDictionary *counted = harness::counters(gDevice);
        EXPECT_TRUE(copied(a, from, size, b, to)) << what;
        EXPECT_EQ(harness::skippedMoved(counted, harness::counters(gDevice)), harness::skippedOnce("call.blit"))
            << what;
        EXPECT_EQ(bytesOf(t), applesCopy(c.s, src, c.d, was, from, size, to)) << what;
        CFRelease(s);
        CFRelease(t);
    }
}

// gpu.gm20b.planes_blits c_b3a8_rgba16u, and plane 0 counted in 16-byte texels: (2,2) 8x4 runs past a row of four or
// eight such texels into the rows after it, where an M4 Max reads them, whether the rows are padded or not; from (8,4)
// its last rows run on past plane 0 into the alpha plane
TEST_F(FacadePlaneCopies, AWiderTexelReadsPlaneZeroAsOneRunWhateverItsPadding)
{
    struct Case {
        NSUInteger pitch;
        MTLPixelFormat format;
        NSUInteger bytes, x, y;
    };

    for (const Case c : {Case{64, MTLPixelFormatRGBA16Uint, 8, 2, 2}, Case{64, MTLPixelFormatRGBA32Uint, 16, 2, 2},
                         Case{128, MTLPixelFormatRGBA32Uint, 16, 2, 2}, Case{192, MTLPixelFormatRGBA32Uint, 16, 2, 2},
                         Case{64, MTLPixelFormatRGBA32Uint, 16, 8, 4}}) {
        const std::string what = "rows of " + std::to_string(c.pitch) + " into a texel of " + std::to_string(c.bytes) +
                                 " from (" + std::to_string(c.x) + "," + std::to_string(c.y) + ")";
        IOSurfaceRef s = xrSurface(c.pitch);
        ASSERT_NE(s, nullptr);
        id<MTLTexture> planes = over(s, 550);
        id<MTLBuffer> rows =
            [gDevice newBufferWithLength:kWidth * c.bytes * kHeight options:MTLResourceStorageModeShared];
        id<MTLTexture> plain = plainOver(rows, c.format, c.bytes);
        const std::vector<uint8_t> before = seeded(s, uint32_t(c.pitch * c.bytes));
        EXPECT_TRUE(copied(planes, MTLOriginMake(c.x, c.y, 0), MTLSizeMake(8, 4, 1), plain, MTLOriginMake(1, 1, 0)))
            << what;
        expectOneRun(before, c.pitch, c.x, c.y, rows, c.bytes, what);
        EXPECT_EQ(bytesOf(s), before) << what << ": the planes are only read";
        CFRelease(s);
    }
}

// A region whose run of bytes ends past its surface's last page, where an M4 Max reads zeros, is Metal's validation
// error (the formats differ) and copies nothing; one a few rows up, inside the page, is moved
TEST_F(FacadePlaneCopies, ARunPastTheSurfacesLastPageCopiesNothing)
{
    IOSurfaceRef s = onePlaneSurface(uint32_t('2vuy'), 16384 / 32);  // one page of 32-byte rows
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(IOSurfaceGetAllocSize(s), 16384u);
    id<MTLTexture> planes = over(s, 501);
    id<MTLBuffer> rows = [gDevice newBufferWithLength:kWidth * 16 * kHeight options:MTLResourceStorageModeShared];
    id<MTLTexture> plain = plainOver(rows, MTLPixelFormatRGBA32Uint, 16);
    const std::vector<uint8_t> before = seeded(s, 501);
    EXPECT_FALSE(copied(planes, MTLOriginMake(8, 508, 0), MTLSizeMake(8, 4, 1), plain, MTLOriginMake(1, 1, 0)))
        << "its run ends at byte 16608 of a 16384-byte surface";
    for (NSUInteger i = 0; i < rows.length; ++i)
        ASSERT_EQ(static_cast<const uint8_t *>(rows.contents)[i], 0xee) << i;
    EXPECT_TRUE(copied(planes, MTLOriginMake(2, 500, 0), MTLSizeMake(8, 4, 1), plain, MTLOriginMake(1, 1, 0)));
    expectOneRun(before, 32, 2, 500, rows, 16, "rows 500..503, the run inside the page");
    CFRelease(s);
}

// YCBCR10_444_1P and its sRGB twin count a word a texel of their one plane, as RGB10A8_2P_XR10 does; YCBCR8_422_1P_REV
// two bytes, as YCBCR8_422_1P does: a copy into a buffer, into a plain texture of a wider texel, and into a texture of
// its own format, where a one-plane texture has no plane 1 to move
TEST_F(FacadePlaneCopies, OnePlaneFormatsCountTheirOwnUnit)
{
    struct Kind {
        uint32_t format, fourcc;
        NSUInteger unit;
    };

    for (const Kind k : {Kind{504, uint32_t('v410'), 4}, Kind{524, uint32_t('v410'), 4}, Kind{562, uint32_t('2vuy'), 2},
                         Kind{501, uint32_t('2vuy'), 2}}) {
        const std::string what = std::to_string(k.format);
        const NSUInteger pitch = kWidth * k.unit;
        IOSurfaceRef s = onePlaneSurface(k.fourcc, kHeight), d = onePlaneSurface(k.fourcc, kHeight);
        ASSERT_TRUE(s && d) << what;
        id<MTLTexture> planes = over(s, k.format), into = over(d, k.format);
        std::vector<uint8_t> before = seeded(s, k.format);

        id<MTLBuffer> out = [gDevice newBufferWithLength:256 options:MTLResourceStorageModeShared];
        memset(out.contents, 0xee, 256);
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:planes
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(2, 1, 0)
                          sourceSize:MTLSizeMake(4, 2, 1)
                            toBuffer:out
                   destinationOffset:0
              destinationBytesPerRow:128
            destinationBytesPerImage:256];
        [e endEncoding];
        EngineRun run;
        ASSERT_NO_FATAL_FAILURE(commitOnRaw(cb, run)) << what;
        const auto *got = static_cast<const uint8_t *>(out.contents);
        for (NSUInteger y = 0; y < 2; ++y)
            for (NSUInteger x = 0; x < 128; ++x)
                ASSERT_EQ(got[y * 128 + x], x < 4 * k.unit ? before[(1 + y) * pitch + 2 * k.unit + x] : 0xee)
                    << what << " into a buffer: byte " << x << " of row " << y;

        id<MTLBuffer> rows = [gDevice newBufferWithLength:kWidth * 8 * kHeight options:MTLResourceStorageModeShared];
        id<MTLTexture> plain = plainOver(rows, MTLPixelFormatRGBA16Uint, 8);
        EXPECT_TRUE(copied(planes, MTLOriginMake(2, 2, 0), MTLSizeMake(8, 4, 1), plain, MTLOriginMake(1, 1, 0)))
            << what;
        expectOneRun(before, pitch, 2, 2, rows, 8, what + " into RGBA16Uint");

        const std::vector<uint8_t> was = seeded(d, k.format + 1);
        EXPECT_TRUE(copied(planes, MTLOriginMake(2, 2, 0), MTLSizeMake(8, 4, 1), into, MTLOriginMake(1, 1, 0))) << what;
        const std::vector<uint8_t> now = bytesOf(d);
        size_t wrong = 0;
        for (NSUInteger at = 0; at < now.size(); ++at) {
            const NSUInteger y = at / pitch, x = at % pitch;
            const bool in = y >= 1 && y < 5 && x >= k.unit && x < 9 * k.unit;
            wrong += now[at] != (in ? before[(y + 1) * pitch + x + k.unit] : was[at]);
        }
        EXPECT_EQ(wrong, 0u) << what << " into its own format: plane 0 in its unit, nothing else";
        CFRelease(s);
        CFRelease(d);
    }
}

// A one-plane source (504 over 'v410', 501 and 562 over '2vuy') into planes of another format: plane 0 moves in the
// destination's unit at each side's rows, and plane 1, which the source has none of, keeps every byte (an M4 Max, 3
// runs: hwtests8 mac/e504/OUTPUT.txt)
TEST_F(FacadePlaneCopies, ASourceOfNoPlaneOneLeavesTheDestinationsAsItIs)
{
    struct Case {
        uint32_t fourcc, format;
        NSUInteger unit;
        TwoPlanes into;
        uint32_t intoFormat;
    };

    const TwoPlanes alpha32{'b3a8', 4, 128, 1024, 1, kWidth, kHeight, 32};
    const TwoPlanes chroma32{'420v', 1, 48, 1024, 2, kWidth / 2, kHeight / 2, 32};
    for (const Case &c : {Case{'v410', 504, 4, alpha32, 550}, Case{'2vuy', 501, 2, chroma32, 500},
                          Case{'2vuy', 562, 2, alpha32, 550}}) {
        const std::string what = std::to_string(c.format) + " into " + std::to_string(c.intoFormat);
        IOSurfaceRef s = onePlaneSurface(c.fourcc, kHeight), d = c.into.make();
        ASSERT_TRUE(s && d) << what;
        id<MTLTexture> one = over(s, c.format), planes = over(d, c.intoFormat);
        const std::vector<uint8_t> src = seeded(s, c.format);
        std::vector<uint8_t> want = seeded(d, c.intoFormat);
        NSDictionary *counted = harness::counters(gDevice);
        EXPECT_TRUE(copied(one, MTLOriginMake(2, 1, 0), MTLSizeMake(4, 4, 1), planes, MTLOriginMake(3, 2, 0))) << what;
        EXPECT_EQ(harness::skippedMoved(counted, harness::counters(gDevice)), harness::skippedOnce("call.blit"))
            << what;
        const NSUInteger unit = c.into.unit0;
        for (NSUInteger y = 0; y < 4; ++y)
            for (NSUInteger b = 0; b < 4 * unit; ++b)
                want[(2 + y) * c.into.rows0 + 3 * unit + b] = src[(1 + y) * kWidth * c.unit + 2 * unit + b];
        EXPECT_EQ(bytesOf(d), want) << what << ": plane 0 in the destination's unit, plane 1 untouched";
        CFRelease(s);
        CFRelease(d);
    }
}

// An M4 Max fills RGB10A8_2P_XR10 planes with a word that ignores the colour and follows the process's earlier fills,
// as it fills YCbCr planes (m2v-blits-fix probes/extra.mm): undefined, recorded as nothing
TEST_F(FacadePlaneCopies, AColourFillOfTwoPlaneXrPlanesWritesNothing)
{
    IOSurfaceRef s = xrSurface(64);
    ASSERT_NE(s, nullptr);
    id<MTLTexture> planes = over(s, 550);
    const std::vector<uint8_t> before = seeded(s, 550);
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    [(id)e fillTexture:planes
                 level:0
                 slice:0
                region:MTLRegionMake2D(5, 1, 6, 3)
                 color:MTLClearColorMake(0.25, 0.5, 0.75, 1)];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded);
    [e endEncoding];
    EngineRun run;
    ASSERT_NO_FATAL_FAILURE(commitOnRaw(cb, run));
    EXPECT_EQ(bytesOf(s), before);
    CFRelease(s);
}
