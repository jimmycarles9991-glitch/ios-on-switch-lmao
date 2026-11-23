// SPDX-License-Identifier: MIT
// Plain formats over surface planes whose rows overlap, lie under 16 bytes or run past the allocation, every texel size
// from 1 to 16 bytes: an M4 Max makes each (validation on and off) and reads each row at the plane's bytesPerRow rounded
// down to 16, rows under 16 bytes 1 MiB apart, nothing past the last page (the plain-rows probe, macOS 27.2.1). The raw
// path makes them through their pools, and stores a pass's rows from the pool by the overlap state of the texel's size.
#include "facade_pass_scene.h"
#include "../plain_rows.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include <string>
#include <vector>

namespace {

using namespace pass_scene;

struct Plane {
    unsigned width, height, bpe, pitch, offset;
};

IOSurfaceRef surface(uint32_t fourcc, unsigned width, unsigned height, unsigned alloc, std::vector<Plane> planes)
{
    NSMutableArray *info = [NSMutableArray array];
    for (const Plane &p : planes)
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

id<MTLDevice> device(bool overlapStates)
{
    std::vector<std::pair<std::string, MTLPixelFormat>> states;
    if (overlapStates)
        states = {{"raw.ycbcr_store.ycbcr_overlap_r8_frag", MTLPixelFormatR8Unorm},
                  {"raw.ycbcr_store.ycbcr_overlap_r16_frag", MTLPixelFormatR16Unorm},
                  {"raw.ycbcr_store.ycbcr_overlap_r32ui_frag", MTLPixelFormatR32Uint},
                  {"raw.ycbcr_store.ycbcr_overlap_rg32ui_frag", MTLPixelFormatRG32Uint},
                  {"raw.ycbcr_store.ycbcr_overlap_rgba32ui_frag", MTLPixelFormatRGBA32Uint}};
    kernel_model::model.infoFlags = TegraGM20BInfoSparse;
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", states));
}

SwitchMetalTexture *texture(id<MTLDevice> device, IOSurfaceRef s, MTLPixelFormat format, NSUInteger width,
                            NSUInteger height)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    return (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
}

MTLRenderPassDescriptor *cleared(SwitchMetalTexture *t)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    return p;
}

struct Shape {
    const char *what;
    MTLPixelFormat format;
    unsigned width, height, pitch, bytes;
};

// The turned-extent probe's R8 136 wide over a '420v' luma plane of rows 16, the plain-rows probe's shapes of 2, 8 and
// 16 bytes a texel, and the turned-extent probe's BGRA8 66 wide over a one-plane 'BGRA' of rows 256
const Shape kShapes[] = {
    {"R8Unorm 136x104 over rows 16", MTLPixelFormatR8Unorm, 136, 104, 16, 1},
    {"R16Uint 136x40 over rows 48", MTLPixelFormatR16Uint, 136, 40, 48, 2},
    {"RGBA16Float 40x70 over rows 160", MTLPixelFormatRGBA16Float, 40, 70, 160, 8},
    {"RGBA32Float 40x40 over rows 128", MTLPixelFormatRGBA32Float, 40, 40, 128, 16},
};

IOSurfaceRef planesOf(const Shape &c)
{
    const unsigned luma = c.pitch * c.height, chroma = (luma + 16383) & ~16383u;
    return surface('420v', c.pitch / c.bytes, c.height, chroma + 16384,
                   {{c.pitch / c.bytes, c.height, c.bytes, c.pitch, 0}, {1, 1, 2, 16, chroma}});
}

void overlappingRowsAreHeldThroughPools()
{
    id<MTLDevice> d = device(true);
    ASSERT_TRUE(d != nil);
    for (const Shape &c : kShapes) {
        IOSurfaceRef s = planesOf(c);
        ASSERT_TRUE(s) << c.what;
        SwitchMetalTexture *t = texture(d, s, c.format, c.width, c.height);
        CFRelease(s);
        ASSERT_TRUE(t != nil && t->_gm20bOk) << c.what << ": an M4 Max makes it";
        const gm20b::Texture &x = t->_raw;
        EXPECT_EQ(x.pitch, c.pitch) << c.what << ": read at the plane's rows";
        ASSERT_TRUE(x.sampled && x.storeView) << c.what << ": held through its pool";
        EXPECT_EQ(x.tic, x.sampled->tic) << c.what;
        EXPECT_EQ(x.storeView->format, gm20b::plainRowsFormat(c.bytes)) << c.what;
        EXPECT_EQ(x.storeView->va, x.sampled->va) << c.what << ": the pool's own memory";
        EXPECT_EQ(t->_cpuPitch, size_t(c.pitch)) << c.what << ": CPU copies at the plane's rows";
        expectRaw(d, run(d, cleared(t)));
    }
    // over a surface of one plane: BGRA8 66 wide over rows of 256 bytes
    IOSurfaceRef s = surface('BGRA', 64, 48, 256 * 48, {{64, 48, 4, 256, 0}});
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = texture(d, s, MTLPixelFormatBGRA8Unorm, 66, 48);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.storeView);
    expectRaw(d, run(d, cleared(t)));
}

// An RGBA32 40x40 over rows of 128 bytes, each texel's rows rendered with its own code: the row whose code each byte two
// or more rows reach holds on an M4 Max (the plain-rows probe, 3 runs alike; elements 8 to 343 of 16 bytes, '0' + row)
constexpr const char *kAppleRows =
    "00000000222222222222222200000000111111112222222233333333444444445555555566666666777777778888888899999999::::::::"
    ";;;;;;;;<<<<<<<<========>>>>>>>>????????@@@@@@@@AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDEEEEEEEEFFFFFFFFGGGGGGGGHHHHHHHH"
    "IIIIIIIIJJJJJJJJKKKKKKKKLLLLLLLLMMMMMMMMNNNNNNNNOOOOOOOOPPPPPPPPQQQQQQQQRRRRRRRRSSSSSSSSTTTTTTTTUUUUUUUUVVVVVVVV";

void storesKeepTheRowAppleLandsLast()
{
    id<MTLDevice> d = device(true);
    ASSERT_TRUE(d != nil);
    const Shape c = {"RGBA32Uint 40x40 over rows 128", MTLPixelFormatRGBA32Uint, 40, 40, 128, 16};
    IOSurfaceRef s = planesOf(c);
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = texture(d, s, c.format, c.width, c.height);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.storeView);
    gm20b::Texture scratch;
    const gm20b::OverlapRows order = gm20b::plainRowsOrder(t->_raw, gm20b::storedRows(t->_raw, t->_raw, scratch));
    const std::string apple = kAppleRows;
    int misses = 0;
    for (uint32_t k = 0; k < apple.size(); k++) {
        uint32_t row = 0, offset = 0;
        const bool stored = gm20b::overlapWinner(
            order, (8 + k) * 16,
            [](uint32_t, uint32_t) {
                return true;
            },
            row, offset);
        misses += !stored || row != uint32_t(apple[k] - '0');
    }
    EXPECT_EQ(misses, 0) << "of " << apple.size() << " elements two or more rows reach";
}

// A whole copy from a buffer into RGBA32Uint 40x40 over rows of 128 bytes: through the pool, its rows then stored by
// the overlap state (an M4 Max's copies land by 32x32 tiles, not in row order): with the state in the pack the
// command buffer draws into the plane's rows; without it, it fails
id<MTLCommandBuffer> copiedWhole(id<MTLDevice> d, SwitchMetalTexture *t)
{
    const Shape c = {"RGBA32Uint 40x40 over rows 128", MTLPixelFormatRGBA32Uint, 40, 40, 128, 16};
    id<MTLBuffer> codes = [d newBufferWithLength:c.width * c.height * c.bytes options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [[d newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:codes
               sourceOffset:0
          sourceBytesPerRow:c.width * c.bytes
        sourceBytesPerImage:c.width * c.height * c.bytes
                 sourceSize:MTLSizeMake(c.width, c.height, 1)
                  toTexture:(id<MTLTexture>)t
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

SwitchMetalTexture *rows128(id<MTLDevice> d)
{
    const Shape c = {"RGBA32Uint 40x40 over rows 128", MTLPixelFormatRGBA32Uint, 40, 40, 128, 16};
    IOSurfaceRef s = planesOf(c);
    if (!s)
        return nil;
    SwitchMetalTexture *t = texture(d, s, c.format, c.width, c.height);
    CFRelease(s);
    return t;
}

void aWholeCopyDrawsTheRowsFromThePool()
{
    id<MTLDevice> d = device(true);
    ASSERT_TRUE(d != nil);
    SwitchMetalTexture *t = rows128(d);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.storeView);
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(d, copiedWhole(d, t));
    bool drawn = false;
    uint64_t address = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        ASSERT_GE(count, 0);
        for (long k = 0; k < count; ++k) {
            const auto &m = methods[k];
            if (m.subchannel != gm20b::SUBC_3D)
                continue;
            if (m.method == NVB197_SET_COLOR_TARGET_A(0))
                address = uint64_t(m.value) << 32;
            else if (m.method == NVB197_SET_COLOR_TARGET_B(0))
                drawn |= (address | m.value) == t->_raw.va;
        }
    }
    EXPECT_TRUE(drawn) << "the plane's rows are a pass's target";
}

void aWholeCopyWithoutTheStateFails()
{
    id<MTLDevice> d = device(false);
    ASSERT_TRUE(d != nil);
    SwitchMetalTexture *t = rows128(d);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.storeView);
    EXPECT_EQ(copiedWhole(d, t).status, MTLCommandBufferStatusError) << "its rows are stored by the overlap state";
}

// The rows a whole copy from a buffer of every texel's code into RGBA32Uint 40x40 over rows of 128 bytes leaves in
// elements 8 to 343 on an M4 Max (ten runs alike, macOS 27.2.1; '0' + row), which the overlap state's copy order keeps
constexpr const char *kAppleCopiedRows =
    "000000002222222222222222444444444444444466666666666666668888888888888888::::::::::::::::<<<<<<<<<<<<<<<<>>>>>>>>"
    ">>>>>>>>@@@@@@@@@@@@@@@@BBBBBBBBBBBBBBBBDDDDDDDDDDDDDDDDFFFFFFFFFFFFFFFFHHHHHHHHHHHHHHHHJJJJJJJJJJJJJJJJLLLLLLLL"
    "LLLLLLLLNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNOOOOOOOOOOOOOOOOTTTTTTTTTTTTTTTTVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVWWWWWWWW";

void copiesKeepTheRowAppleLandsLast()
{
    id<MTLDevice> d = device(true);
    ASSERT_TRUE(d != nil);
    SwitchMetalTexture *t = rows128(d);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.storeView);
    ASSERT_TRUE(gm20b::copiedThroughPool(t->_raw, 0, 0, 40, 40));
    EXPECT_FALSE(gm20b::copiedThroughPool(t->_raw, 0, 0, 39, 40)) << "a part keeps the copy engine's rows";
    gm20b::Texture scratch;
    gm20b::OverlapRows order = gm20b::plainRowsOrder(t->_raw, gm20b::storedRows(t->_raw, t->_raw, scratch));
    order.copied = 1;
    const std::string apple = kAppleCopiedRows;
    int misses = 0;
    for (uint32_t k = 0; k < apple.size(); k++) {
        uint32_t row = 0, offset = 0;
        const bool stored = gm20b::overlapWinner(
            order, (8 + k) * 16,
            [](uint32_t, uint32_t) {
                return true;
            },
            row, offset);
        misses += !stored || row != uint32_t(apple[k] - '0');
    }
    EXPECT_EQ(misses, 0) << "of " << apple.size() << " elements two or more rows reach";
}

void aPackWithoutTheStateFailsThePass()
{
    id<MTLDevice> d = device(false);
    ASSERT_TRUE(d != nil);
    IOSurfaceRef s = planesOf(kShapes[3]);
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = texture(d, s, kShapes[3].format, kShapes[3].width, kShapes[3].height);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.storeView);
    EXPECT_EQ(run(d, cleared(t)).status, MTLCommandBufferStatusError) << "its rows are stored by the overlap state";
}

// The turned-extent probe's 1048 rows over a one-plane 'BGRA' of 48 rows of 256 bytes, and R8 over a '420v' luma plane of
// rows 8, which an M4 Max reads 1 MiB apart and stores 16 apart (rows past the last page read 0: a sparse window)
void rowsPastThePageAndUnderSixteen()
{
    id<MTLDevice> d = device(true);
    ASSERT_TRUE(d != nil);
    IOSurfaceRef s = surface('BGRA', 64, 48, 256 * 48, {{64, 48, 4, 256, 0}});
    ASSERT_TRUE(s);
    SwitchMetalTexture *t = texture(d, s, MTLPixelFormatBGRA8Unorm, 64, 1048);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "an M4 Max makes it";
    EXPECT_GE(t->_raw.window.bytes, uint64_t(256) * 1047 + 256) << "its rows read through a window";
    EXPECT_LE(t->_raw.storage.bytes, t->_raw.window.pages) << "a job names the bound pages alone";
    EXPECT_EQ(t->_raw.storeView, nullptr) << "rows that do not overlap need no pool";
    expectRaw(d, run(d, cleared(t)));
    s = surface('420v', 8, 6, 32768, {{8, 6, 1, 8, 0}, {4, 3, 2, 8, 16384}});
    ASSERT_TRUE(s);
    t = texture(d, s, MTLPixelFormatR8Unorm, 24, 6);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "an M4 Max makes it";
    EXPECT_EQ(t->_raw.pitch, 1u << 20) << "read 1 MiB apart";
    EXPECT_EQ(t->_raw.storePitch, 16u) << "stored 16 apart";
    EXPECT_EQ(t->_cpuPitch, size_t(8)) << "CPU copies at the plane's rows";
    EXPECT_TRUE(t->_raw.window.bytes && t->_raw.storeView);
    expectRaw(d, run(d, cleared(t)));
}

// Whether the surface step's nil line (gm20b:tex.surf.imp) was logged in this process
bool importStepLogged()
{
    return !switch_refusal_first(switch_refusal_key_with(switch_refusal_key("gm20b", "tex.surf.imp"), 0));
}

// Rows past the last page on a kernel that maps no VA sparse (no TegraGM20BInfoSparse): nil, the import's step
void noSparseNoRowsPastThePage()
{
    kernel_model::model.infoFlags = 0;
    id<MTLDevice> d = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}));
    ASSERT_TRUE(d != nil);
    IOSurfaceRef s = surface('BGRA', 64, 48, 256 * 48, {{64, 48, 4, 256, 0}});
    ASSERT_TRUE(s);
    EXPECT_EQ(texture(d, s, MTLPixelFormatBGRA8Unorm, 64, 1048), nil);
    CFRelease(s);
    EXPECT_TRUE(importStepLogged());
}

// A one-plane surface a render target made first, which the kernel lays out block-linear: its rows no longer lie as the
// plane's, so a texture over them that overlap is nil, the import's step
void tiledFirstNoRowsThatOverlap()
{
    kernel_model::model.infoFlags = TegraGM20BInfoSparse | TegraGM20BInfoSurfaceLayout;
    kernel_model::model.surfaceLayout = TegraGM20BLayoutTiled;
    id<MTLDevice> d = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}));
    ASSERT_TRUE(d != nil);
    IOSurfaceRef s = surface('BGRA', 64, 48, 256 * 48, {{64, 48, 4, 256, 0}});
    ASSERT_TRUE(s);
    SwitchMetalTexture *target = texture(d, s, MTLPixelFormatBGRA8Unorm, 64, 48);
    ASSERT_TRUE(target != nil && target->_gm20bOk && target->_raw.blockLinear) << "the kernel tiled it for the target";
    EXPECT_EQ(texture(d, s, MTLPixelFormatBGRA8Unorm, 66, 48), nil);
    CFRelease(s);
    EXPECT_TRUE(importStepLogged());
}

}  // namespace

TEST(GM20BFacadePlainRows, WithoutSparseRowsPastTheLastPageAreNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(noSparseNoRowsPastThePage);
}

TEST(GM20BFacadePlainRows, OverRowsTheKernelTiledRowsThatOverlapAreNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(tiledFirstNoRowsThatOverlap);
}

TEST(GM20BFacadePlainRows, PlainFormatsOverRowsThatOverlapAreHeldThroughTheirPools)
{
    switch_ios::test::ExpectPassesInFreshProcess(overlappingRowsAreHeldThroughPools);
}

TEST(GM20BFacadePlainRows, APassesStoresKeepTheRowAnM4MaxLandsLast)
{
    switch_ios::test::ExpectPassesInFreshProcess(storesKeepTheRowAppleLandsLast);
}

TEST(GM20BFacadePlainRows, AWholeCopyIntoWideTexelsDrawsItsRowsFromThePool)
{
    switch_ios::test::ExpectPassesInFreshProcess(aWholeCopyDrawsTheRowsFromThePool);
}

TEST(GM20BFacadePlainRows, AWholeCopyIntoWideTexelsWithoutTheOverlapStateFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(aWholeCopyWithoutTheStateFails);
}

TEST(GM20BFacadePlainRows, ACopysStoresKeepTheRowAnM4MaxLandsLast)
{
    switch_ios::test::ExpectPassesInFreshProcess(copiesKeepTheRowAppleLandsLast);
}

TEST(GM20BFacadePlainRows, APackWithoutTheOverlapStateFailsThePassesIntoThem)
{
    switch_ios::test::ExpectPassesInFreshProcess(aPackWithoutTheStateFailsThePass);
}

TEST(GM20BFacadePlainRows, RowsPastTheLastPageOrUnderSixteenBytesAreMade)
{
    switch_ios::test::ExpectPassesInFreshProcess(rowsPastThePageAndUnderSixteen);
}
