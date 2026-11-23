// SPDX-License-Identifier: MIT
// Copies between RGB10A8_2P_XR10 textures one of which a descriptor made, as the facade records them and the copy
// engine's model runs them (facade_blit_engines.h), against an M4 Max's rule (m2v-textures13 probes/c1copies,
// RESULT.txt H-d: 287 cases, 0 misses): each side's words and alpha bytes addressed at row * Pb + unit * x, a surface's
// Pb its plane 0's rows rounded down to 16, a descriptor's its words' row rounded down to 16 (16 under four texels) over
// rows of words and of alpha each 16-byte aligned, past which bytes read 0 and take no write; a descriptor under four
// texels wide gives its row 0 alone as a source. The model below is that rule over plain bytes, written apart from the
// driver's.
#include "facade_blit_engines.h"
#include "../TegraGM20BMetalInternal.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace harness;

id<MTLDevice> gDevice;

class FacadeXrCopies : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                 {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                                  {"raw.xr.xr_planes_frag", MTLPixelFormatRGBA16Float}}));
        ASSERT_TRUE(gDevice != nil);
    }
};

// A side of a copy as the rule addresses it: a surface's bytes (planes at 0 and 1024, plane 0's rows 128) or a
// descriptor's texels, a word and an alpha byte each
struct XrBytes {
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> words, alpha;
    uint32_t width = 0, height = 0;
    bool surface = false;

    uint32_t pitch() const
    {
        return surface ? 128 : 4 * width < 16 ? 16 : (4 * width) & ~15u;
    }

    // byte `at` of plane `p` (0 words, 1 alpha): its place, false past the side's bytes
    bool place(uint32_t p, uint64_t at, uint64_t &index, uint32_t &shift) const
    {
        if (surface) {
            index = (p ? 1024 : 0) + at;
            shift = 0;
            return index < bytes.size();
        }
        const uint32_t unit = p ? 1 : 4, row = unit * width, stored = (row + 15) & ~15u;
        if (at / stored >= height || at % stored >= row)
            return false;
        index = at / stored * width + at % stored / unit;
        shift = 8 * uint32_t(at % unit);
        return true;
    }

    uint8_t read(uint32_t p, uint64_t at, bool rowZeroOnly) const
    {
        uint64_t i;
        uint32_t shift;
        if (!place(p, at, i, shift) || (rowZeroOnly && i >= width))
            return 0;
        return surface ? bytes[i] : uint8_t((p ? alpha : words)[i] >> shift);
    }

    void write(uint32_t p, uint64_t at, uint8_t v)
    {
        uint64_t i;
        uint32_t shift;
        if (!place(p, at, i, shift))
            return;
        if (surface) {
            bytes[i] = v;
            return;
        }
        uint32_t &x = (p ? alpha : words)[i];
        x = (x & ~(0xffu << shift)) | uint32_t(v) << shift;
    }
};

void applesCopy(const XrBytes &s, MTLOrigin from, MTLSize size, XrBytes &d, MTLOrigin to)
{
    for (uint32_t p = 0; p < 2; p++) {
        const uint32_t unit = p ? 1 : 4;
        for (NSUInteger j = 0; j < size.height; j++)
            for (NSUInteger b = 0; b < size.width * unit; b++)
                d.write(p, (to.y + j) * d.pitch() + unit * to.x + b,
                        s.read(p, (from.y + j) * s.pitch() + unit * from.x + b, !s.surface && 4 * s.width < 16));
    }
}

// A 'b3a8' surface of 32x8: plane 0 words in rows of 128 at 0, plane 1 alpha in rows of 32 at 1024, every byte at random
IOSurfaceRef xrSurface(XrBytes &side, uint32_t seed)
{
    NSDictionary *words = @{
        (id)kIOSurfacePlaneWidth : @32,
        (id)kIOSurfacePlaneHeight : @8,
        (id)kIOSurfacePlaneBytesPerElement : @4,
        (id)kIOSurfacePlaneBytesPerRow : @128,
        (id)kIOSurfacePlaneOffset : @0,
        (id)kIOSurfacePlaneSize : @1024
    };
    NSDictionary *alphas = @{
        (id)kIOSurfacePlaneWidth : @32,
        (id)kIOSurfacePlaneHeight : @8,
        (id)kIOSurfacePlaneBytesPerElement : @1,
        (id)kIOSurfacePlaneBytesPerRow : @32,
        (id)kIOSurfacePlaneOffset : @1024,
        (id)kIOSurfacePlaneSize : @256
    };
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @16384,
        (id)kIOSurfacePlaneInfo : @[ words, alphas ]
    });
    if (!s)
        return nullptr;
    std::mt19937 rng(seed);
    auto *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(s));
    for (size_t i = 0; i < IOSurfaceGetAllocSize(s); i++)
        base[i] = uint8_t(rng());
    side = {};
    side.surface = true;
    side.width = 32;
    side.height = 8;
    side.bytes.assign(base, base + IOSurfaceGetAllocSize(s));
    return s;
}

id<MTLTexture> overSurface(IOSurfaceRef s)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550) width:32 height:8 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [gDevice newTextureWithDescriptor:d iosurface:s plane:0];
    EXPECT_TRUE([(id)t isKindOfClass:[SwitchMetalTexture class]] && ((SwitchMetalTexture *)t)->_gm20bOk &&
                ((SwitchMetalTexture *)t)->_raw.xrPlanes);
    return t;
}

// A descriptor's 550 of `width` x `height`, every texel's word and alpha byte at random (its texels' eight bytes: the
// word, then the alpha byte in a word of its own)
SwitchMetalTexture *descriptor(uint32_t width, uint32_t height, XrBytes &side, uint32_t seed)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550)
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    d.storageMode = MTLStorageModeShared;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[gDevice newTextureWithDescriptor:d];
    EXPECT_TRUE(t != nil && t->_gm20bOk);
    std::mt19937 rng(seed);
    side = {};
    side.width = width;
    side.height = height;
    side.words.resize(size_t(width) * height);
    side.alpha.resize(side.words.size());
    std::vector<uint32_t> texels(2 * side.words.size());
    for (size_t i = 0; i < side.words.size(); i++) {
        texels[2 * i] = side.words[i] = uint32_t(rng());
        texels[2 * i + 1] = side.alpha[i] = uint32_t(rng()) & 0xff;
    }
    [t gm20bReplaceRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0
                    slice:0
                withBytes:texels.data()
              bytesPerRow:8 * width
            bytesPerImage:0];
    return t;
}

void expectHolds(SwitchMetalTexture *t, const XrBytes &want, const std::string &what)
{
    std::vector<uint32_t> texels(2 * size_t(want.width) * want.height);
    [t gm20bGetBytes:texels.data()
          bytesPerRow:8 * want.width
        bytesPerImage:0
           fromRegion:MTLRegionMake2D(0, 0, want.width, want.height)
          mipmapLevel:0
                slice:0];
    size_t wrong = 0, first = SIZE_MAX;
    for (size_t i = 0; i < want.words.size(); i++)
        if (texels[2 * i] != want.words[i] || texels[2 * i + 1] != want.alpha[i]) {
            wrong++;
            first = std::min(first, i);
        }
    EXPECT_EQ(wrong, 0u) << what << ": first wrong texel " << first;
}

void expectHolds(IOSurfaceRef s, const XrBytes &want, const std::string &what)
{
    const auto *got = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(s));
    size_t wrong = 0, first = SIZE_MAX;
    for (size_t i = 0; i < want.bytes.size(); i++)
        if (got[i] != want.bytes[i]) {
            wrong++;
            first = std::min(first, i);
        }
    EXPECT_EQ(wrong, 0u) << what << ": first wrong byte " << first;
}

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

}  // namespace

// A surface's planes into a descriptor's texels (refused before): words and alpha bytes land where the descriptor's
// rows of words and of alpha hold the rule's addresses, row padding taking none; widths of 17 (words a row at 64 of 80),
// 16 (alpha region row j on alpha row 4j, rows past the texture dropped) and 2 (16 a row both)
TEST_F(FacadeXrCopies, ASurfaceIntoADescriptorLandsAtTheRulesAddresses)
{
    struct Case {
        uint32_t width, height;
        MTLOrigin from;
        MTLSize size;
        MTLOrigin to;
    };

    for (const Case &c : {Case{17, 4, {3, 1, 0}, {13, 3, 1}, {2, 0, 0}}, Case{16, 8, {0, 0, 0}, {16, 8, 1}, {0, 0, 0}},
                          Case{2, 8, {5, 2, 0}, {2, 6, 1}, {0, 1, 0}}, Case{32, 8, {5, 0, 0}, {8, 2, 1}, {3, 0, 0}}}) {
        const std::string what = "into " + std::to_string(c.width) + "x" + std::to_string(c.height);
        XrBytes s, d;
        IOSurfaceRef surface = xrSurface(s, c.width);
        ASSERT_TRUE(surface) << what;
        SwitchMetalTexture *t = descriptor(c.width, c.height, d, c.width + 100);
        EXPECT_TRUE(copied(overSurface(surface), c.from, c.size, (id<MTLTexture>)t, c.to)) << what;
        applesCopy(s, c.from, c.size, d, c.to);
        expectHolds(t, d, what);
        expectHolds(surface, s, what + ": the source is only read");
        CFRelease(surface);
    }
}

// A descriptor's texels into a surface's planes (moved in 8-byte units before): bytes the rule finds in a descriptor's
// row padding or past its rows read 0, and a descriptor under four texels wide gives its row 0 alone
TEST_F(FacadeXrCopies, ADescriptorIntoASurfaceReadsTheRulesAddresses)
{
    struct Case {
        uint32_t width, height;
        MTLOrigin from;
        MTLSize size;
        MTLOrigin to;
    };

    for (const Case &c : {Case{17, 4, {1, 1, 0}, {16, 3, 1}, {4, 2, 0}}, Case{16, 8, {0, 0, 0}, {16, 8, 1}, {0, 0, 0}},
                          Case{2, 8, {0, 0, 0}, {2, 8, 1}, {7, 0, 0}}, Case{32, 8, {5, 0, 0}, {8, 2, 1}, {3, 0, 0}}}) {
        const std::string what = "from " + std::to_string(c.width) + "x" + std::to_string(c.height);
        XrBytes s, d;
        SwitchMetalTexture *t = descriptor(c.width, c.height, s, c.width);
        IOSurfaceRef surface = xrSurface(d, c.width + 100);
        ASSERT_TRUE(surface) << what;
        EXPECT_TRUE(copied((id<MTLTexture>)t, c.from, c.size, overSurface(surface), c.to)) << what;
        applesCopy(s, c.from, c.size, d, c.to);
        expectHolds(surface, d, what);
        CFRelease(surface);
    }
}

// Between two descriptors (whole texels before): each side at its own pitch, 17 wide into 33 wide
TEST_F(FacadeXrCopies, DescriptorsMoveAtEachOnesPitch)
{
    XrBytes s, d;
    SwitchMetalTexture *a = descriptor(17, 4, s, 7), *b = descriptor(33, 5, d, 8);
    EXPECT_TRUE(copied((id<MTLTexture>)a, MTLOriginMake(0, 0, 0), MTLSizeMake(16, 4, 1), (id<MTLTexture>)b,
                       MTLOriginMake(1, 1, 0)));
    applesCopy(s, MTLOriginMake(0, 0, 0), MTLSizeMake(16, 4, 1), d, MTLOriginMake(1, 1, 0));
    expectHolds(b, d, "17x4 into 33x5");
    expectHolds(a, s, "the source is only read");
}
