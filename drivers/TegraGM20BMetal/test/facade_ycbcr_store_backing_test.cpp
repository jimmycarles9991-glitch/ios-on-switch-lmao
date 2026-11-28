// SPDX-License-Identifier: MIT
// A YCbCr or packed-plane target's store passes read the memoryless stand-in its pass rendered, a backing of the pass's
// transient colour tag; backings of one tag share memory. A plane rendered through a pool image (rows not 128 bytes
// apart: Texture::shadowTarget) copies its bytes into that image before the store draws, so the image must lie outside
// the stand-in: at the transient tag the copy put the plane's own bytes where the store then read the stand-in (HW Test
// gpu.packed_plane_target 588 over 'p420' plane 0, gpu.gm20b.ycbcr_target_shapes plane_one).
#include "facade_pass_scene.h"
#include "kits/SwitchGM20B/SwitchGM20B3D.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "../push.h"

namespace {

using namespace pass_scene;

constexpr NSUInteger kWidth = 128, kHeight = 16, kPitch = 176;

// The colour targets' addresses of each pass from job `first` on, in push order
std::vector<uint64_t> colourTargets(size_t first)
{
    std::vector<uint64_t> out;
    uint64_t address[8] = {};
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        for (long k = 0; k < count; ++k) {
            const auto &m = methods[k];
            if (m.subchannel != gm20b::SUBC_3D)
                continue;
            for (uint32_t j = 0; j < 8; ++j) {
                if (m.method == NVB197_SET_COLOR_TARGET_A(j))
                    address[j] = uint64_t(m.value) << 32;
                else if (m.method == NVB197_SET_COLOR_TARGET_B(j))
                    address[j] |= m.value;
                else if (m.method == NVB197_SET_COLOR_TARGET_FORMAT(j) && m.value)
                    out.push_back(address[j]);
            }
        }
    }
    return out;
}

// 'p420' 128x16: plane 0 of 43 words a row, rows 176 bytes apart, then plane 1 (described in bytes and pairs, as
// facade_surface_forms_test's 'p420' is)
IOSurfaceRef packed420()
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned plane : {0u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(kWidth >> plane),
            (id)kIOSurfacePlaneHeight : @(kHeight >> plane),
            (id)kIOSurfacePlaneBytesPerElement : @(plane + 1),
            (id)kIOSurfacePlaneBytesPerRow : @(kPitch),
            (id)kIOSurfacePlaneOffset : @(plane ? 4096 : 0),
            (id)kIOSurfacePlaneSize : @(kPitch * (kHeight >> plane))
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kWidth),
        (id)kIOSurfaceHeight : @(kHeight),
        (id)kIOSurfacePixelFormat : @(uint32_t('p420')),
        (id)kIOSurfaceAllocSize : @(8192),
        (id)kIOSurfacePlaneInfo : planes
    });
}

void theStorePassesPoolImageLiesOutsideTheStandInItReads()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_packed_plane_frag", MTLPixelFormatRGB10A2Unorm},
                                           {"raw.ycbcr_store.ycbcr_store_plane_packed_frag", MTLPixelFormatR32Uint}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = packed420();
    ASSERT_TRUE(s);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(588)
                                                                                 width:kWidth
                                                                                height:kHeight
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.luma);
    ASSERT_NE(t->_raw.planarStandIn, nullptr) << "a target's memoryless stand-in";
    ASSERT_TRUE(t->_raw.luma->shadowTarget) << "rows 176 bytes apart render through a pool image";
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p));
    const std::vector<uint64_t> targets = colourTargets(first);
    ASSERT_GE(targets.size(), 2u) << "the pass into the stand-in, then the store's into the plane's pool image";
    const uint64_t standIn = targets.front();
    for (size_t i = 1; i < targets.size(); ++i)
        EXPECT_NE(targets[i], standIn) << "the store pass's target lands in the stand-in it reads (pass " << i << ")";
}

// '_RB4' 128x16 whose luma rows lie 144 bytes apart and chroma rows 272: both planes render through pool images
IOSurfaceRef fourFourFourOffRows()
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned plane : {0u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(kWidth),
            (id)kIOSurfacePlaneHeight : @(kHeight),
            (id)kIOSurfacePlaneBytesPerElement : @(plane + 1),
            (id)kIOSurfacePlaneBytesPerRow : @(plane ? 272 : 144),
            (id)kIOSurfacePlaneOffset : @(plane ? 4096 : 0),
            (id)kIOSurfacePlaneSize : @((plane ? 272 : 144) * kHeight)
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kWidth),
        (id)kIOSurfaceHeight : @(kHeight),
        (id)kIOSurfacePixelFormat : @(uint32_t('_RB4')),
        (id)kIOSurfaceAllocSize : @(12288),
        (id)kIOSurfacePlaneInfo : planes
    });
}

SwitchMetalTexture *target(id<MTLDevice> device, MTLPixelFormat format, IOSurfaceRef s)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:kWidth height:kHeight mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
    CFRelease(s);
    return t;
}

// A planar (RGB8_444_2P) target at colour 0 and a 588 at colour 1: the planar one's store pass runs first at the pass's
// end, its planes through pool images, before the 588's store reads its memoryless stand-in, the Metal pass's first
// transient colour backing. At the transient tags the planar store's luma image took that backing's memory.
void aPlanarStoreBeforeAYcbcrStoreLeavesItsStandInAlone()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.two_plane.two_plane_load", MTLPixelFormatRGBA16Float},
                                           {"raw.two_plane.two_plane_store", MTLPixelFormatR8Unorm},
                                           {"raw.ycbcr.ycbcr_packed_plane_frag", MTLPixelFormatRGB10A2Unorm},
                                           {"raw.ycbcr_store.ycbcr_store_plane_packed_frag", MTLPixelFormatR32Uint}}));
    ASSERT_TRUE(device != nil);
    SwitchMetalTexture *planar = target(device, MTLPixelFormat(542), fourFourFourOffRows());
    SwitchMetalTexture *packed = target(device, MTLPixelFormat(588), packed420());
    ASSERT_TRUE(planar != nil && planar->_gm20bOk && planar->_raw.planar && planar->_raw.planarStandIn);
    ASSERT_TRUE(planar->_raw.luma && planar->_raw.luma->shadowTarget)
        << "rows 144 bytes apart render through a pool image";
    ASSERT_TRUE(packed != nil && packed->_gm20bOk && packed->_raw.planarStandIn);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    for (NSUInteger j : {0u, 1u}) {
        p.colorAttachments[j].texture = (id<MTLTexture>)(j ? packed : planar);
        p.colorAttachments[j].loadAction = MTLLoadActionClear;
        p.colorAttachments[j].storeAction = MTLStoreActionStore;
        p.colorAttachments[j].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    }
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p));
    const std::vector<uint64_t> targets = colourTargets(first);
    ASSERT_GE(targets.size(), 4u) << "the pass into both stand-ins, then the planar store's and the 588's";
    const uint64_t standIn = targets[1];
    for (size_t i = 2; i < targets.size(); ++i)
        EXPECT_NE(targets[i], standIn) << "a store pass's target lands in the 588's stand-in (target " << i << ")";
}

}  // namespace

TEST(GM20BFacadeYcbcrStoreBacking, APlanarStoreBeforeAYcbcrStoreLeavesItsStandInAlone)
{
    switch_ios::test::ExpectPassesInFreshProcess(aPlanarStoreBeforeAYcbcrStoreLeavesItsStandInAlone);
}

TEST(GM20BFacadeYcbcrStoreBacking, TheStorePassesPoolImageLiesOutsideTheStandInItReads)
{
    switch_ios::test::ExpectPassesInFreshProcess(theStorePassesPoolImageLiesOutsideTheStandInItReads);
}
