// SPDX-License-Identifier: MIT
// Turned targets over planes whose rows overlap or run into the next plane, each plane declared no wider than its
// bytesPerRow, as IOSurfaceCreate makes it: the surfaces of HW Test gpu.gm20b.ycbcr_turned_overlap_order,
// gpu.gm20b.turned_overlap_tile_order and gpu.gm20b.xr_turned_clear_order. An M4 Max makes each (it checks a turned frame against
// no plane's declared size) and stores it by its frame's 32x32 tiles. The driver makes each at its plane's extent, and
// the order its store states draw by (overlap_order.h), built from the texture made, keeps every byte those tests judge.
#include "facade_pass_scene.h"
#include "../overlap_rows.h"
#include "../xr_pools.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace {

using namespace pass_scene;

struct PlaneSpec {
    unsigned width, height, bpe, pitch, offset;
};

IOSurfaceRef surface(uint32_t fourcc, unsigned width, unsigned height, unsigned alloc,
                     std::initializer_list<PlaneSpec> planes)
{
    NSMutableArray *info = [NSMutableArray array];
    for (const PlaneSpec &p : planes)
        [info addObject:@{
            (id)kIOSurfacePlaneWidth : @(p.width),
            (id)kIOSurfacePlaneHeight : @(p.height),
            (id)kIOSurfacePlaneBytesPerElement : @(p.bpe),
            (id)kIOSurfacePlaneBytesPerRow : @(p.pitch),
            (id)kIOSurfacePlaneOffset : @(p.offset),
            (id)kIOSurfacePlaneSize : @(p.pitch * p.height)
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(width),
        (id)kIOSurfaceHeight : @(height),
        (id)kIOSurfacePixelFormat : @(fourcc),
        (id)kIOSurfaceAllocSize : @(alloc),
        (id)kIOSurfacePlaneInfo : info
    });
}

SwitchMetalTexture *turnedTarget(id<MTLDevice> device, IOSurfaceRef s, uint32_t format, NSUInteger width,
                                 NSUInteger height, NSUInteger turn)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    [d setValue:@YES forKey:@"framebufferOnly"];
    [d setValue:@(turn) forKey:@"rotation"];
    return (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
}

id<MTLDevice> ycbcrDevice(const char *conversion, MTLPixelFormat image)
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                          {{conversion, image},
                           {"raw.ycbcr_store.ycbcr_store_luma8_frag", MTLPixelFormatR8Unorm},
                           {"raw.ycbcr_store.ycbcr_store_chroma8_frag", MTLPixelFormatRG8Unorm},
                           {"raw.ycbcr_store.ycbcr_overlap_r8_frag", MTLPixelFormatR8Unorm},
                           {"raw.ycbcr_store.ycbcr_overlap_rg8_frag", MTLPixelFormatRG8Unorm}}));
}

id<MTLDevice> xrDevice()
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                          {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                           {gm20b::xrTargetState(550, gm20b::XrStep::Load, 1, true), MTLPixelFormatRGBA16Float},
                           {gm20b::xrTargetState(550, gm20b::XrStep::Store, 1, true), MTLPixelFormatR32Uint},
                           {gm20b::kOverlapPlanesState, MTLPixelFormatR8Uint}}));
}

// A Clear pass into `t` (drawing nothing) on the driver
void clearRaw(id<MTLDevice> device, SwitchMetalTexture *t)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, -0.25, 0.75);
    expectRaw(device, run(device, p));
}

// YcbcrTurnedOverlapOrder.frameTexel: plane texel (px, py) of a frame w x h turned by `turn`
void deviceFrame(int turn, int w, int h, int px, int py, int &fx, int &fy)
{
    fx = turn == 1 ? py : turn == 2 ? w - 1 - px : turn == 3 ? w - 1 - py : px;
    fy = turn == 1 ? h - 1 - px : turn == 2 ? h - 1 - py : turn == 3 ? px : py;
}

// gpu.gm20b.ycbcr_turned_overlap_order's judge of luma rows 136 bytes long and 16 apart: each byte some reaching row
// cannot win holds a frame tile's winner (the later row pair, a pair's even row after its odd one); `s` picks the row
void ycbcrTurnedOrder(int turn)
{
    id<MTLDevice> device = ycbcrDevice("raw.ycbcr.ycbcr_codes_frag", MTLPixelFormatRGBA8Unorm);
    ASSERT_TRUE(device != nil);
    constexpr int kW = 136, kH = 104, kPitch = 16;
    IOSurfaceRef s = surface('420v', kW, kH, 32768, {{16, kH, 1, kPitch, 0}, {8, kH / 2, 2, kPitch, 16384}});
    ASSERT_TRUE(s);
    const int fw = turn & 1 ? kH : kW, fh = turn & 1 ? kW : kH;
    SwitchMetalTexture *t = turnedTarget(device, s, 540, NSUInteger(fw), NSUInteger(fh), NSUInteger(turn));
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "an M4 Max makes it at rotation " << turn;
    ASSERT_TRUE(t->_raw.luma && t->_raw.luma->sampled && t->_raw.planarStandIn);
    EXPECT_EQ(int(t->_raw.rotation), turn);
    EXPECT_EQ(t->_raw.width, uint32_t(kW)) << "made at its plane's extent";
    EXPECT_EQ(t->_raw.luma->pitch, uint32_t(kPitch));
    gm20b::Texture scratch;
    const gm20b::Texture &rows = gm20b::storedRows(*t->_raw.luma, t->_raw, scratch);
    gm20b::OverlapRows order = gm20b::overlapPlaneRows(rows, false, *t->_raw.ycbcrStore);
    order.tiles = gm20b::frameTiles(t->_raw);
    int judged = 0, misses = 0;
    for (int a = 0; a < kPitch * (kH - 1) + kW; a++) {
        const int last = std::min(a / kPitch, kH - 1), first = a >= kW ? (a - kW) / kPitch + 1 : 0;
        if (last <= first)
            continue;
        std::map<int, std::pair<int, int>> best;
        std::set<int> codes, winners;
        for (int r = first; r <= last; r++) {
            int fx = 0, fy = 0;
            deviceFrame(turn, fw, fh, a - r * kPitch, r, fx, fy);
            const int code = (fx + 7 * fy) & 255, key = (fy % 32 / 2) << 1 | (fy % 2 == 0 ? 1 : 0);
            const int tile = (fx / 32) << 16 | fy / 32;
            codes.insert(code);
            if (!best.count(tile) || best[tile].first < key)
                best[tile] = {key, code};
        }
        for (const auto &b : best)
            winners.insert(b.second.second);
        if (std::includes(winners.begin(), winners.end(), codes.begin(), codes.end()))
            continue;
        judged++;
        uint32_t row = 0, offset = 0;
        int fx = -1, fy = -1;
        if (gm20b::overlapWinner(
                order, uint32_t(a),
                [](uint32_t, uint32_t) {
                    return true;
                },
                row, offset))
            deviceFrame(turn, fw, fh, int(offset), int(row), fx, fy);
        misses += fx < 0 || !winners.count((fx + 7 * fy) & 255);
    }
    EXPECT_EQ(judged, 1744);
    EXPECT_EQ(misses, 0) << "luma bytes holding no frame tile's winner at rotation " << turn;
    clearRaw(device, t);
}

void ycbcrTurnedOrderOne()
{
    ycbcrTurnedOrder(1);
}

void ycbcrTurnedOrderTwo()
{
    ycbcrTurnedOrder(2);
}

void ycbcrTurnedOrderThree()
{
    ycbcrTurnedOrder(3);
}

// gpu.gm20b.turned_overlap_tile_order's chroma_rows_r3: a 500 frame 96x128 turned by 3 over a '420v' of luma 128x96 and chroma
// 64x48 pairs, both rows 16 apart: chroma bytes 16-31 hold chroma row 0's bytes 16-31, bytes 848-863 row 46's 112-127
void chromaRowsTurnedThree()
{
    id<MTLDevice> device = ycbcrDevice("raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float);
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = surface('420v', 128, 96, 32768, {{16, 96, 1, 16, 0}, {8, 48, 2, 16, 16384}});
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = turnedTarget(device, s, 500, 96, 128, 3);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "an M4 Max makes it";
    ASSERT_TRUE(t->_raw.chroma && t->_raw.chroma->sampled);
    gm20b::Texture scratch;
    const gm20b::Texture &rows = gm20b::storedRows(*t->_raw.chroma, t->_raw, scratch);
    gm20b::OverlapRows order = gm20b::overlapPlaneRows(rows, true, *t->_raw.ycbcrStore);
    order.tiles = gm20b::frameTiles(t->_raw);
    int off = 0;
    for (uint32_t i = 0; i < 16; i++)
        for (const auto &[at, wantRow, wantOffset] : {std::tuple<uint32_t, uint32_t, uint32_t>{16 + i, 0, 16 + i},
                                                      std::tuple<uint32_t, uint32_t, uint32_t>{848 + i, 46, 112 + i}}) {
            uint32_t row = ~0u, offset = ~0u;
            gm20b::overlapWinner(
                order, at,
                [](uint32_t, uint32_t) {
                    return true;
                },
                row, offset);
            off += row != wantRow || offset != wantOffset;
        }
    EXPECT_EQ(off, 0) << "of 32 chroma bytes the device test judges";
    clearRaw(device, t);
}

// Bytes [from, to) of a 550's span each the store of word row `row`'s byte alone, as an M4 Max keeps them
int wordRowBytes(const gm20b::OverlapPlanes &order, uint32_t from, uint32_t to, uint32_t row)
{
    int off = 0;
    for (uint32_t a = from; a < to; a++) {
        gm20b::OverlapWriters out;
        off += !gm20b::overlapPlanesWriters(order, a, out) || out.count != 1 || out.plane[0] != 0 || out.y[0] != row ||
               out.x[0] != (a - row * order.plane[0].pitch) / 4;
    }
    return off;
}

// gpu.gm20b.turned_overlap_tile_order's word_rows_r1: a 550 frame 72x96 turned by 1 over a 'b3a8' of words declared 24 wide,
// rows 96 apart (a row 384 bytes, running into the alpha plane at 6912), drawn: bytes 96-127 hold word row 1's words 0-7
void wordRowsTurnedOne()
{
    id<MTLDevice> device = xrDevice();
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = surface('b3a8', 96, 72, 16384, {{24, 72, 4, 96, 0}, {96, 72, 1, 96, 6912}});
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = turnedTarget(device, s, 550, 72, 96, 1);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "an M4 Max makes it";
    ASSERT_TRUE(t->_raw.xrPlanes && t->_raw.luma && t->_raw.chroma);
    EXPECT_EQ(t->_raw.width, 96u);
    uint64_t lo = 0, hi = 0;
    const gm20b::OverlapPlanes order = gm20b::crossingPlanesOrder(t->_raw, false, lo, hi);
    EXPECT_EQ(hi - lo, 6912u + 96 * 72) << "the words' rows into the alpha plane, to its end";
    EXPECT_EQ(wordRowBytes(order, 96, 128, 1), 0);
}

// gpu.gm20b.xr_turned_clear_order: a 550 frame 8x96 turned by 1 over a 'b3a8' of words declared 48 wide, rows 192 apart (rows
// of 384 bytes running into the alpha plane at 1536), cleared alone: bytes 1568-1597 and 1696-1706 hold word row 7's
void clearOrderTurnedOne()
{
    id<MTLDevice> device = xrDevice();
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = surface('b3a8', 96, 8, 16384, {{48, 8, 4, 192, 0}, {96, 8, 1, 96, 1536}});
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = turnedTarget(device, s, 550, 8, 96, 1);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "an M4 Max makes it";
    uint64_t lo = 0, hi = 0;
    const gm20b::OverlapPlanes order = gm20b::crossingPlanesOrder(t->_raw, true, lo, hi);
    EXPECT_EQ(wordRowBytes(order, 1568, 1598, 7) + wordRowBytes(order, 1696, 1707, 7), 0);
    clearRaw(device, t);
}

}  // namespace

TEST(GM20BFacadeTurnedOverlap, A540TurnedOverLumaRowsSixteenApartKeepsEachFrameTilesWinner)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrTurnedOrderOne);
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrTurnedOrderTwo);
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrTurnedOrderThree);
}

TEST(GM20BFacadeTurnedOverlap, A500TurnedThreeKeepsTheChromaRowAppleKeeps)
{
    switch_ios::test::ExpectPassesInFreshProcess(chromaRowsTurnedThree);
}

TEST(GM20BFacadeTurnedOverlap, A550TurnedOneOverNarrowWordsKeepsTheLaterWordRow)
{
    switch_ios::test::ExpectPassesInFreshProcess(wordRowsTurnedOne);
}

TEST(GM20BFacadeTurnedOverlap, A550TurnedOneClearedKeepsTheWordOverTheAlpha)
{
    switch_ios::test::ExpectPassesInFreshProcess(clearOrderTurnedOne);
}
