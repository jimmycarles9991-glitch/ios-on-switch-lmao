// SPDX-License-Identifier: MIT
// A YCbCr target whose planes' stored rows meet: HW Test gpu.gm20b.ycbcr_planes_meet's surface, 500 8x8 over a '420v' of luma
// rows 8 at 0 and chroma rows 8 at 64, both stored 16 apart, so chroma row k lands on luma row 4 + k. An M4 Max keeps the
// later 2x2 frame quad's store there and the OR of one quad's luma and chroma bytes (5 runs). The driver holds both planes
// in pools its stores render, and the pack's meet state draws their span by that order; a pack without the state has no
// target of it, whose passes fail.
#include "facade_pass_scene.h"
#include "../ycbcr_meet.h"
#include <algorithm>
#include <tuple>
#include <vector>

namespace {

using namespace pass_scene;

IOSurfaceRef meetingPlanes()
{
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
            (id)kIOSurfacePlaneOffset : @64,
            (id)kIOSurfacePlaneSize : @32
        }
    ];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @8,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('420v')),
        (id)kIOSurfaceAllocSize : @((8u << 20) + 16384),
        (id)kIOSurfacePlaneInfo : planes
    });
}

id<MTLDevice> device(bool meetState)
{
    std::vector<std::pair<std::string, MTLPixelFormat>> states = {
        {"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
        {"raw.ycbcr_store.ycbcr_store_luma8_frag", MTLPixelFormatR8Unorm},
        {"raw.ycbcr_store.ycbcr_store_chroma8_frag", MTLPixelFormatRG8Unorm}};
    if (meetState)
        states.push_back({gm20b::kOverlapMeetState, MTLPixelFormatR8Uint});
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", states));
}

SwitchMetalTexture *target(id<MTLDevice> device, IOSurfaceRef s)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(500) width:8 height:8 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    return (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
}

MTLRenderPassDescriptor *cleared(SwitchMetalTexture *t)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.8, 0.4, 0.2, 1);
    return p;
}

void planesThatMeetAreStoredByTheirQuads()
{
    id<MTLDevice> d = device(true);
    ASSERT_TRUE(d != nil);
    IOSurfaceRef s = meetingPlanes();
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = target(d, s);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.luma && t->_raw.chroma) << "an M4 Max makes it";
    ASSERT_NE(t->_raw.planarStandIn, nullptr) << "a target";
    ASSERT_TRUE(t->_raw.luma->sampled && t->_raw.chroma->sampled)
        << "both planes held in the pools their stores render";
    ASSERT_TRUE(gm20b::ycbcrPlanesMeet(t->_raw, *t->_raw.ycbcrStore));
    uint64_t lo = 0, hi = 0;
    const gm20b::OverlapPlanes order = gm20b::meetingPlanesOrder(t->_raw, nullptr, lo, hi);
    EXPECT_EQ(lo, t->_raw.luma->va);
    EXPECT_EQ(hi - lo, 64u + 16 * 3 + 8) << "luma's rows 16 apart to chroma row 3's end";
    using Store = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
    for (uint32_t k = 0; k < 4; k++)
        for (uint32_t b = 0; b < 8; b++) {
            gm20b::OverlapWriters w;
            ASSERT_TRUE(gm20b::overlapPlanesWriters(order, 64 + 16 * k + b, w));
            std::vector<Store> got;
            for (uint32_t j = 0; j < w.count; j++)
                got.emplace_back(w.plane[j], w.x[j], w.y[j], w.byte[j]);
            const std::vector<Store> want = k < 3 ? std::vector<Store>{Store(0, b, 4 + k, 0)}
                                                  : std::vector<Store>{Store(0, b, 7, 0), Store(1, b / 2, 3, b % 2)};
            EXPECT_EQ(got, want) << "chroma row " << k << " byte " << b;
        }
    expectRaw(d, run(d, cleared(t)));
}

void noMeetStateNoTarget()
{
    id<MTLDevice> d = device(false);
    ASSERT_TRUE(d != nil);
    IOSurfaceRef s = meetingPlanes();
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = target(d, s);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk);
    EXPECT_EQ(t->_raw.planarStandIn, nullptr) << "no target its pack cannot store";
    EXPECT_EQ(run(d, cleared(t)).status, MTLCommandBufferStatusError);
}

}  // namespace

TEST(GM20BFacadeYcbcrMeet, PlanesWhoseStoredRowsMeetAreStoredByTheirFramesQuads)
{
    switch_ios::test::ExpectPassesInFreshProcess(planesThatMeetAreStoredByTheirQuads);
}

TEST(GM20BFacadeYcbcrMeet, APackWithoutTheMeetStateFailsThePassesIntoIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(noMeetStateNoTarget);
}
