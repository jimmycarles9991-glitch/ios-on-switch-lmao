// SPDX-License-Identifier: MIT
// An RGB8_444_2P (542) surface texture made to be sampled alone, as Core Animation's widget snapshots are, holds no stand-in;
// a pass into it renders on the driver all the same (an M4 Max renders into a texture of any usage as into a render
// target: harness/ycbcrnorenderoracle.mm, m2v-passes2 probe/nort.mm), through a stand-in of its command buffer's. A
// one-plane YCbCr texture renders through its memoryless stand-in into its luma and chroma images, merged into its pairs; one
// whose plane rows overlap draws each plane's bytes from its stored rows.
#include "facade_pass_scene.h"
#include "../push.h"
#include <algorithm>

namespace {

using namespace pass_scene;

constexpr NSUInteger kSide = 64;

// '_RB4': a byte of luma and two of chroma a texel
IOSurfaceRef fourFourFour()
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {1u, 2u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(kSide),
            (id)kIOSurfacePlaneHeight : @(kSide),
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(kSide * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 1 ? 0 : 16384),
            (id)kIOSurfacePlaneSize : @(kSide * kSide * bytes)
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('_RB4')),
        (id)kIOSurfaceAllocSize : @(16384 + kSide * kSide * 2),
        (id)kIOSurfacePlaneInfo : planes
    });
}

id<MTLDevice> planarDevice()
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                          {{"raw.two_plane.two_plane_load", MTLPixelFormatRGBA16Float},
                           {"raw.two_plane.two_plane_store", MTLPixelFormatR8Unorm}}));
}

SwitchMetalTexture *planarTexture(id<MTLDevice> device, IOSurfaceRef s, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(542)
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = usage;
    return (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
}

void aSampledPlanarTextureRenders(MTLLoadAction load)
{
    id<MTLDevice> device = planarDevice();
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = fourFourFour();
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = planarTexture(device, s, MTLTextureUsageShaderRead);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.planar);
    EXPECT_EQ(t->_raw.planarStandIn, nullptr) << "a texture made to be sampled holds no stand-in";
    EXPECT_EQ(t->_raw.luma, nullptr);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = load;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    expectRaw(device, run(device, p));
    EXPECT_EQ(t->_raw.planarStandIn, nullptr) << "nor after its pass: the stand-in was the command buffer's";
}

// The texture made with render-target usage keeps the stand-in it holds, and two passes into a sampled one and a target
// one in one command buffer each render through their own
void aSampledAndATargetPlanarTextureRenderInOneCommandBuffer()
{
    id<MTLDevice> device = planarDevice();
    ASSERT_TRUE(device != nil);
    IOSurfaceRef a = fourFourFour(), b = fourFourFour();
    ASSERT_TRUE(a && b);
    SwitchMetalTexture *sampled = planarTexture(device, a, MTLTextureUsageShaderRead),
                       *target = planarTexture(device, b, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    CFRelease(a);
    CFRelease(b);
    ASSERT_TRUE(sampled != nil && target != nil && sampled->_gm20bOk && target->_gm20bOk);
    ASSERT_NE(target->_raw.planarStandIn, nullptr);
    gm20b::Texture *held = target->_raw.planarStandIn;
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    for (SwitchMetalTexture *t : {sampled, target}) {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = (id<MTLTexture>)t;
        p.colorAttachments[0].loadAction = MTLLoadActionLoad;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[1].texture = (id<MTLTexture>)(t == sampled ? target : sampled);
        p.colorAttachments[1].loadAction = MTLLoadActionClear;
        p.colorAttachments[1].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    }
    [cb commit];
    [cb waitUntilCompleted];
    expectRaw(device, cb);
    EXPECT_EQ(target->_raw.planarStandIn, held);
    EXPECT_EQ(sampled->_raw.planarStandIn, nullptr);
}

// The low address word each copy-engine launch since job `first` wrote to
std::vector<uint32_t> copyDestinations(size_t first)
{
    constexpr uint32_t kOutLower = 0x040c, kLaunchDma = 0x0300;
    std::vector<uint32_t> out;
    uint32_t to = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            const auto method = [&](uint32_t at, uint32_t v) {
                if (subc != gm20b::SUBC_COPY)
                    return;
                if (at == kOutLower)
                    to = v;
                else if (at == kLaunchDma)
                    out.push_back(to);
            };
            if (op == 4) {
                method(m, count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i)
                method(op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i]);
        }
    }
    return out;
}

// A one-plane YCbCr texture (501 over a '2vuy' surface) cleared and stored renders on the driver: its stand-in stored by
// the two-plane 4:2:2 store states into its luma and chroma images, then merged into its pairs by the copy engine
// (BlitEncoder::mergePlane: two launches whose destination is the surface's plane)
void aOnePlaneYcbcrTargetRenders()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.ycbcr_store.ycbcr_store_luma8_frag", MTLPixelFormatR8Unorm},
                                           {"raw.ycbcr_store.ycbcr_store_chroma8_frag", MTLPixelFormatRG8Unorm}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('2vuy')),
        (id)kIOSurfaceBytesPerElement : @2,
        (id)kIOSurfaceBytesPerRow : @(2 * kSide),
        (id)kIOSurfaceAllocSize : @(2 * kSide * kSide)
    });
    ASSERT_TRUE(s);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(501)
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.onePlane);
    ASSERT_NE(t->_raw.planarStandIn, nullptr) << "a target's memoryless stand-in";
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p));
    const std::vector<uint32_t> to = copyDestinations(first);
    EXPECT_EQ(std::count(to.begin(), to.end(), uint32_t(t->_raw.planes.luma)), 2)
        << "luma's and chroma's copies into the pairs";
}

// A YCbCr target whose plane rows overlap (540 64x16 over a '420v' of rows 48 bytes apart, rows of 64 luma bytes and of 64
// chroma bytes) renders on the driver: its rows stored into each plane's pool image, then each plane's bytes drawn from it
// by the pack's overlap states (overlap_rows.h): the run of its whole pitches and the bytes past it, each a target the
// copy engine drains into the plane (rows 48 bytes apart are no linear target)
void aYcbcrTargetWhoseRowsOverlapRenders()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_codes_frag", MTLPixelFormatRGBA8Unorm},
                                           {"raw.ycbcr_store.ycbcr_store_luma8_frag", MTLPixelFormatR8Unorm},
                                           {"raw.ycbcr_store.ycbcr_store_chroma8_frag", MTLPixelFormatRG8Unorm},
                                           {"raw.ycbcr_store.ycbcr_overlap_r8_frag", MTLPixelFormatR8Unorm},
                                           {"raw.ycbcr_store.ycbcr_overlap_rg8_frag", MTLPixelFormatRG8Unorm}}));
    ASSERT_TRUE(device != nil);
    NSArray *planes = @[
        @{
            (id)kIOSurfacePlaneWidth : @48,
            (id)kIOSurfacePlaneHeight : @16,
            (id)kIOSurfacePlaneBytesPerElement : @1,
            (id)kIOSurfacePlaneBytesPerRow : @48,
            (id)kIOSurfacePlaneOffset : @0,
            (id)kIOSurfacePlaneSize : @4096
        },
        @{
            (id)kIOSurfacePlaneWidth : @24,
            (id)kIOSurfacePlaneHeight : @8,
            (id)kIOSurfacePlaneBytesPerElement : @2,
            (id)kIOSurfacePlaneBytesPerRow : @48,
            (id)kIOSurfacePlaneOffset : @4096,
            (id)kIOSurfacePlaneSize : @4096
        }
    ];
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @48,
        (id)kIOSurfaceHeight : @16,
        (id)kIOSurfacePixelFormat : @(uint32_t('420v')),
        (id)kIOSurfaceAllocSize : @8192,
        (id)kIOSurfacePlaneInfo : planes
    });
    ASSERT_TRUE(s);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(540) width:64 height:16 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.luma && t->_raw.chroma);
    ASSERT_NE(t->_raw.planarStandIn, nullptr) << "a target's memoryless stand-in (planYcbcr took the target)";
    ASSERT_TRUE(t->_raw.luma->sampled && t->_raw.chroma->sampled) << "the pool images the rows are stored into";
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p));
    const std::vector<uint32_t> to = copyDestinations(first);
    const uint64_t luma = t->_raw.luma->va, chroma = t->_raw.chroma->va;
    const auto drained = [&](uint64_t va) {
        return std::count(to.begin(), to.end(), uint32_t(va)) > 0;
    };
    EXPECT_TRUE(drained(luma)) << "luma's 16 whole pitches (784 bytes of rows)";
    EXPECT_TRUE(drained(luma + 16 * 48)) << "luma's last 16 bytes";
    EXPECT_TRUE(drained(chroma)) << "chroma's 8 whole pitches (400 bytes of rows)";
    EXPECT_TRUE(drained(chroma + 8 * 48)) << "chroma's last 16 bytes";
}

// A YCbCr target of rows under 16 bytes, which an M4 Max reads 1 MiB apart and stores 16 apart (m2v-textures12
// probes/f2/targets-full.txt #45, #46): its store states render each plane's rows 16 apart, which no linear render target
// is, so through a backing the copy engine drains into the plane
void aYcbcrTargetOfRowsUnderSixteenBytesStoresThemSixteenApart()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_codes_frag", MTLPixelFormatRGBA8Unorm},
                                           {"raw.ycbcr_store.ycbcr_store_luma8_frag", MTLPixelFormatR8Unorm},
                                           {"raw.ycbcr_store.ycbcr_store_chroma8_frag", MTLPixelFormatRG8Unorm}}));
    ASSERT_TRUE(device != nil);
    NSArray *planes = @[
        @{
            (id)kIOSurfacePlaneWidth : @8,
            (id)kIOSurfacePlaneHeight : @8,
            (id)kIOSurfacePlaneBytesPerElement : @1,
            (id)kIOSurfacePlaneBytesPerRow : @8,
            (id)kIOSurfacePlaneOffset : @0,
            (id)kIOSurfacePlaneSize : @64
        },
        @{
            (id)kIOSurfacePlaneWidth : @4,
            (id)kIOSurfacePlaneHeight : @4,
            (id)kIOSurfacePlaneBytesPerElement : @2,
            (id)kIOSurfacePlaneBytesPerRow : @8,
            (id)kIOSurfacePlaneOffset : @4096,
            (id)kIOSurfacePlaneSize : @32
        }
    ];
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @8,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('420v')),
        (id)kIOSurfaceAllocSize : @((8u << 20) + 16384),
        (id)kIOSurfacePlaneInfo : planes
    });
    ASSERT_TRUE(s);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(540) width:8 height:8 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.luma && t->_raw.chroma);
    EXPECT_EQ(t->_raw.luma->pitch, 1u << 20) << "read 1 MiB apart";
    EXPECT_EQ(t->_raw.luma->storePitch, 16u);
    EXPECT_EQ(t->_raw.chroma->storePitch, 16u);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p));
    const std::vector<uint32_t> to = copyDestinations(first);
    for (const gm20b::Texture *plane : {t->_raw.luma, t->_raw.chroma})
        EXPECT_GT(std::count(to.begin(), to.end(), uint32_t(plane->va)), 0) << "its rows drained 16 apart";
}

}  // namespace

TEST(GM20BFacadePlanarTargets, AYcbcrTargetOfRowsUnderSixteenBytesStoresThemSixteenApart)
{
    switch_ios::test::ExpectPassesInFreshProcess(aYcbcrTargetOfRowsUnderSixteenBytesStoresThemSixteenApart);
}

TEST(GM20BFacadePlanarTargets, AOnePlaneYcbcrTargetRendersIntoItsPairs)
{
    switch_ios::test::ExpectPassesInFreshProcess(aOnePlaneYcbcrTargetRenders);
}

TEST(GM20BFacadePlanarTargets, AYcbcrTargetWhoseRowsOverlapRendersIntoItsPlanesBytes)
{
    switch_ios::test::ExpectPassesInFreshProcess(aYcbcrTargetWhoseRowsOverlapRenders);
}

TEST(GM20BFacadePlanarTargets, ASampledPlanarTextureClearedAndStoredRenders)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        aSampledPlanarTextureRenders(MTLLoadActionClear);
    });
}

TEST(GM20BFacadePlanarTargets, ASampledPlanarTextureLoadedAndStoredRenders)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        aSampledPlanarTextureRenders(MTLLoadActionLoad);
    });
}

TEST(GM20BFacadePlanarTargets, ASampledAndATargetPlanarTextureRenderInOneCommandBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(aSampledAndATargetPlanarTextureRenderInOneCommandBuffer);
}
