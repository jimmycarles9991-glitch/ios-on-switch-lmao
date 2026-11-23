// SPDX-License-Identifier: MIT
// R10Unorm_PACKED over a surface's plane on the facade over the kernel model, as an M4 Max makes and reads it
// (m2v-textures11 probes/h4/mips.txt): getBytes writes nothing at any level; of several levels, every lod reads level 0,
// its image drawn from the plane's words and then each level from level 0's corner by the copy engine, ahead of the pass
// that samples it through its sampler's level-0 header. Each scenario is a process of its own.
#include "facade_harness.h"
#include "bazel/testing/fresh_process.h"
#include "../push.h"
#include "../sampler_map.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using namespace harness;

constexpr MTLPixelFormat kR10Packed = MTLPixelFormat(588);

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

// A 'p420' 32x16 of rows 64: plane 0 at 0, plane 1 (16x8 pairs) after it
IOSurfaceRef p420()
{
    NSMutableArray *planes = [NSMutableArray array];
    for (const unsigned plane : {0u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(32 >> plane),
            (id)kIOSurfacePlaneHeight : @(16 >> plane),
            (id)kIOSurfacePlaneBytesPerElement : @(plane ? 4 : 2),
            (id)kIOSurfacePlaneElementWidth : @1,
            (id)kIOSurfacePlaneBytesPerRow : @64,
            (id)kIOSurfacePlaneOffset : @(plane * 1024),
            (id)kIOSurfacePlaneSize : @(64 * (16 >> plane))
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @16,
        (id)kIOSurfacePixelFormat : @(uint32_t('p420')),
        (id)kIOSurfaceAllocSize : @1536,
        (id)kIOSurfacePlaneInfo : planes
    });
}

MTLTextureDescriptor *packed(NSUInteger levels)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kR10Packed width:32 height:16 mipmapped:NO];
    d.mipmapLevelCount = levels;
    d.usage = MTLTextureUsageShaderRead;
    return d;
}

std::string packedPack()
{
    const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
    return writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                     {{"raw.ycbcr.ycbcr_packed_plane_frag", MTLPixelFormatRGB10A2Unorm},
                      {"raw.ycbcr_store.ycbcr_store_plane_packed_frag", MTLPixelFormatR32Uint}},
                     {sampled});
}

// Every byte of getBytes' rows still the sentinel
bool untouched(id<MTLTexture> t, NSUInteger level)
{
    // room for the image words a getBytes once copied (four bytes a texel), so a copy lands in it
    std::vector<uint8_t> rows(2048, 0xee);
    [t getBytes:rows.data()
        bytesPerRow:48
         fromRegion:MTLRegionMake2D(0, 0, std::max<NSUInteger>(32 >> level, 1), std::max<NSUInteger>(16 >> level, 1))
        mipmapLevel:level];
    return std::all_of(rows.begin(), rows.end(), [](uint8_t b) {
        return b == 0xee;
    });
}

// What the jobs pushed, in order: a pass's colour target 0, a copy engine launch's destination
struct Event {
    bool copy;
    uint64_t address;
};

std::vector<Event> events(size_t first)
{
    // NVB197_SET_COLOR_TARGET_A/B/FORMAT(0), NVB0B5_OFFSET_OUT_UPPER/LOWER and LAUNCH_DMA
    constexpr uint32_t kTargetA = 0x0800, kTargetB = 0x0804, kTargetFormat = 0x0810, kOutUpper = 0x0408,
                       kOutLower = 0x040c, kLaunchDma = 0x0300;
    std::vector<Event> out;
    uint64_t target = 0, to = 0;
    const auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc == gm20b::SUBC_3D && m == kTargetA)
            target = (target & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == gm20b::SUBC_3D && m == kTargetB)
            target = (target & ~0xffffffffull) | v;
        else if (subc == gm20b::SUBC_3D && m == kTargetFormat && v)
            out.push_back({false, target});
        else if (subc == gm20b::SUBC_COPY && m == kOutUpper)
            to = (to & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == gm20b::SUBC_COPY && m == kOutLower)
            to = (to & ~0xffffffffull) | v;
        else if (subc == gm20b::SUBC_COPY && m == kLaunchDma)
            out.push_back({true, to});
    };
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            if (op == 4) {
                method(subc, m, count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i)
                method(subc, op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i]);
        }
    }
    return out;
}

size_t indexOf(const std::vector<Event> &e, bool copy, uint64_t address)
{
    for (size_t i = 0; i < e.size(); ++i)
        if (e[i].copy == copy && e[i].address == address)
            return i;
    return SIZE_MAX;
}

// One level: getBytes leaves every byte as it was (it once copied the RGB10A2 image's words, four bytes a texel, past
// rows sized for the format's one)
void oneLevelGetsNothing()
{
    id<MTLDevice> device = open(packedPack());
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = p420();
    ASSERT_TRUE(s);
    id<MTLTexture> t = [device newTextureWithDescriptor:packed(1) iosurface:s plane:0];
    ASSERT_TRUE(t != nil && ((SwitchMetalTexture *)t)->_gm20bOk);
    EXPECT_TRUE(untouched(t, 0));
    CFRelease(s);
}

// Two levels (needs TegraGM20BMetalSurfaceTextures.mm's facts to carry the levels): the image a chain of both; a pass sampling it
// draws level 0 from the words, the copy engine copies level 1 from it, and the pass samples after both through its
// sampler's level-0 header; getBytes of either level writes nothing
void twoLevelsReadLevelZero()
{
    id<MTLDevice> device = open(packedPack());
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    IOSurfaceRef s = p420();
    ASSERT_TRUE(s);
    id<MTLTexture> t = [device newTextureWithDescriptor:packed(2) iosurface:s plane:0];
    ASSERT_TRUE(t != nil && ((SwitchMetalTexture *)t)->_gm20bOk);
    const gm20b::Texture &x = ((SwitchMetalTexture *)t)->_raw;
    ASSERT_TRUE(x.levelZero && x.chain);
    gm20b::Texture level1;
    ASSERT_TRUE(gm20b::chainLevelImage(x, 1, level1));
    MTLSamplerDescriptor *trilinear = [MTLSamplerDescriptor new];
    trilinear.minFilter = trilinear.magFilter = MTLSamplerMinMagFilterLinear;
    trilinear.mipFilter = MTLSamplerMipFilterLinear;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:trilinear];
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = drawSampling(device, state, t, sampler);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    const std::vector<Event> e = events(first);
    const size_t image = indexOf(e, false, x.va), copied = indexOf(e, true, level1.va);
    ASSERT_NE(image, SIZE_MAX) << "the image drawn from the plane's words";
    ASSERT_NE(copied, SIZE_MAX) << "level 1 copied from level 0";
    EXPECT_LT(image, copied);
    EXPECT_EQ(copied + 1 < e.size() && !e[copied + 1].copy, true) << "the sampling pass after the copy";
    EXPECT_NE(((SwitchMetalSampler *)sampler)->_raw.mapped[gm20b::kLevelZeroMap], 0u);
    EXPECT_TRUE(untouched(t, 0));
    EXPECT_TRUE(untouched(t, 1));
    // generateMipmapsForTexture: writes no level, each staying level 0's corner, and counts nothing: an M4 Max's completes and
    // changes nothing (validation on and off: m2v-textures13 probes/pmips/pmips.txt)
    NSDictionary *before = counters(device);
    const size_t generated = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> mips = [[device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [mips blitCommandEncoder];
    [b generateMipmapsForTexture:t];
    [b endEncoding];
    [mips commit];
    [mips waitUntilCompleted];
    EXPECT_EQ(mips.status, MTLCommandBufferStatusCompleted) << text(mips.error);
    EXPECT_TRUE(skippedMoved(before, counters(device)).empty());
    EXPECT_EQ(indexOf(events(generated), false, level1.va), SIZE_MAX);
    CFRelease(s);
}

// HW Test gpu.gm20b.packed_levels_open's views: of R10Unorm_PACKED 32x16 of three levels, views of levels 1-2, 2 and 0,
// which an M4 Max makes at those levels' sizes (the packed-levels views probe, 3 runs): each a header over the
// chain's levels it names, every lod still reading its level 0
void viewsOfItsLevels()
{
    id<MTLDevice> device = open(packedPack());
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = p420();
    ASSERT_TRUE(s);
    MTLTextureDescriptor *d = packed(3);
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> t = [device newTextureWithDescriptor:d iosurface:s plane:0];
    CFRelease(s);
    ASSERT_TRUE(t != nil && ((SwitchMetalTexture *)t)->_gm20bOk && ((SwitchMetalTexture *)t)->_raw.chain);
    const NSUInteger parts[3][2] = {{1, 2}, {2, 1}, {0, 1}};
    for (const auto &p : parts) {
        id<MTLTexture> v = [t newTextureViewWithPixelFormat:kR10Packed
                                                textureType:MTLTextureType2D
                                                     levels:NSMakeRange(p[0], p[1])
                                                     slices:NSMakeRange(0, 1)];
        ASSERT_TRUE(v != nil) << "levels " << p[0] << "+" << p[1];
        EXPECT_TRUE(((SwitchMetalTexture *)v)->_gm20bOk) << "levels " << p[0] << "+" << p[1];
        EXPECT_EQ(v.width, NSUInteger(32 >> p[0]));
        EXPECT_EQ(v.height, NSUInteger(16 >> p[0]));
        EXPECT_EQ(v.mipmapLevelCount, p[1]);
        const gm20b::Texture &x = ((SwitchMetalTexture *)v)->_raw;
        EXPECT_NE(x.tic, 0u);
        EXPECT_EQ(x.width, uint32_t(32 >> p[0])) << "its header over the chain's level " << p[0];
        EXPECT_TRUE(x.levelZero);
    }
}

}  // namespace

TEST(GM20BFacadePackedLevels, ViewsOfItsLevelsAreMadeAtTheirSizes)
{
    switch_ios::test::ExpectPassesInFreshProcess(viewsOfItsLevels);
}

TEST(GM20BFacadePackedLevels, OneLevelGetsNothing)
{
    switch_ios::test::ExpectPassesInFreshProcess(oneLevelGetsNothing);
}

TEST(GM20BFacadePackedLevels, TwoLevelsReadLevelZero)
{
    switch_ios::test::ExpectPassesInFreshProcess(twoLevelsReadLevelZero);
}
