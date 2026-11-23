// SPDX-License-Identifier: MIT
// Metal's texture <-> buffer copies as the facade records them (facade_harness.h): on every texture the driver
// holds, every layout Metal's validation takes (row lengths, image heights, offsets, origins, levels, slices, 3D depth,
// block formats, a depth or stencil aspect) must be the driver's, each direction in a command buffer of its own; what
// Metal refuses itself (a multisampled texture) records nothing and completes on the driver. The textures the driver
// does not hold (their shape or format: rule tex.shape) are listed; their copies are m2v's with them. Copies between
// textures: facade_texture_copies_test.cpp.
#include "facade_blit_engines.h"
#include "../TegraGM20BMetalInternal.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace harness;

struct Layout {
    std::string name;
    MTLPixelFormat format = MTLPixelFormatRGBA8Unorm;
    MTLTextureType type = MTLTextureType2D;
    NSUInteger width = 32, height = 32, depth = 1, levels = 1, layers = 1, samples = 1;
    MTLStorageMode storage = MTLStorageModePrivate;
    NSUInteger level = 0, slice = 0;
    MTLOrigin origin = {0, 0, 0};
    MTLSize size = {32, 32, 1};
    NSUInteger offset = 0, row = 0, image = 0;  // row 0: the size's tight row
    MTLBlitOption options = MTLBlitOptionNone;
    NSUInteger texelBytes = 4, block = 1;  // the buffer's bytes per texel or block, the block's side
    bool target = false;                   // made a render target too, as a depth or stencil texture is
    bool metalRefuses = false;
};

id<MTLDevice> gDevice;
const std::string kNotHeld = "not held";

class FacadeCopies : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_TRUE(gDevice != nil);
    }

    // The recorded refusal mask of a copy each way, nil where the texture is not made
    static std::string refusedDirections(const Layout &l)
    {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = l.type;
        d.pixelFormat = l.format;
        d.width = l.width;
        d.height = l.height;
        d.depth = l.depth;
        d.mipmapLevelCount = l.levels;
        d.arrayLength = l.layers;
        d.sampleCount = l.samples;
        d.storageMode = l.storage;
        d.usage = MTLTextureUsageShaderRead | (l.samples > 1 || l.target ? MTLTextureUsageRenderTarget : 0);
        id<MTLTexture> texture = [gDevice newTextureWithDescriptor:d];
        if (![(id)texture isKindOfClass:[SwitchMetalTexture class]] || !((SwitchMetalTexture *)texture)->_gm20bOk)
            return kNotHeld;
        const NSUInteger blocksWide = (l.size.width + l.block - 1) / l.block,
                         blocksHigh = (l.size.height + l.block - 1) / l.block;
        const NSUInteger row = l.row ? l.row : blocksWide * l.texelBytes;
        const NSUInteger image = l.image ? l.image : row * blocksHigh;
        id<MTLBuffer> buffer =
            [gDevice newBufferWithLength:l.offset + image * l.size.depth + 4096 options:MTLResourceStorageModeShared];
        std::string refused;
        id<MTLCommandQueue> queue = [gDevice newCommandQueue];
        for (bool intoTexture : {true, false}) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
            if (intoTexture)
                [e copyFromBuffer:buffer
                           sourceOffset:l.offset
                      sourceBytesPerRow:row
                    sourceBytesPerImage:image
                             sourceSize:l.size
                              toTexture:texture
                       destinationSlice:l.slice
                       destinationLevel:l.level
                      destinationOrigin:l.origin
                                options:l.options];
            else
                [e copyFromTexture:texture
                                 sourceSlice:l.slice
                                 sourceLevel:l.level
                                sourceOrigin:l.origin
                                  sourceSize:l.size
                                    toBuffer:buffer
                           destinationOffset:l.offset
                      destinationBytesPerRow:row
                    destinationBytesPerImage:image
                                     options:l.options];
            const bool skipped = ((SwitchMetalCommandBuffer *)cb)->_plan.size() == recorded;
            [e endEncoding];
            if (((SwitchMetalCommandBuffer *)cb)->_pred0) {
                refused += intoTexture ? " b2t" : " t2b";
                continue;
            }
            if (skipped)
                refused += intoTexture ? " b2t@skipped" : " t2b@skipped";
            [cb commit];
            [cb waitUntilCompleted];
            NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
            if (cb.status != MTLCommandBufferStatusCompleted || cb.error || ![path isEqualToString:@"gm20b"])
                refused += std::string(intoTexture ? " b2t" : " t2b") + "@encode(" + (path.UTF8String ?: "?") + ")";
        }
        return refused;
    }
};

std::vector<Layout> layouts()
{
    std::vector<Layout> out;
    auto add = [&](std::string name, auto edit) {
        Layout l;
        l.name = std::move(name);
        edit(l);
        out.push_back(l);
    };
    add("tight", [](Layout &) {});
    add("shared storage", [](Layout &l) {
        l.storage = MTLStorageModeShared;
    });
    add("padded row", [](Layout &l) {
        l.row = 32 * 4 + 64;
    });
    add("row padded by a texel", [](Layout &l) {
        l.row = 33 * 4;
    });
    add("offset of a texel", [](Layout &l) {
        l.offset = 4;
    });
    add("offset of 12", [](Layout &l) {
        l.offset = 12;
    });
    add("one row, any row length", [](Layout &l) {
        l.size = {32, 1, 1};
        l.row = 0;
        l.offset = 8;
    });
    add("sub-rect at an origin", [](Layout &l) {
        l.origin = {5, 7, 0};
        l.size = {13, 9, 1};
    });
    add("odd width", [](Layout &l) {
        l.width = l.height = 33;
        l.size = {33, 33, 1};
    });
    add("level 1", [](Layout &l) {
        l.levels = 6;
        l.level = 1;
        l.size = {16, 16, 1};
    });
    add("last level", [](Layout &l) {
        l.levels = 6;
        l.level = 5;
        l.size = {1, 1, 1};
    });
    add("array slice 1", [](Layout &l) {
        l.type = MTLTextureType2DArray;
        l.layers = 3;
        l.slice = 1;
    });
    add("array slice 1 level 1", [](Layout &l) {
        l.type = MTLTextureType2DArray;
        l.layers = 2;
        l.levels = 3;
        l.slice = 1;
        l.level = 1;
        l.size = {16, 16, 1};
    });
    add("cube face 4", [](Layout &l) {
        l.type = MTLTextureTypeCube;
        l.slice = 4;
    });
    add("3D, four planes", [](Layout &l) {
        l.type = MTLTextureType3D;
        l.depth = 8;
        l.origin = {0, 0, 2};
        l.size = {32, 32, 4};
    });
    add("3D, padded image", [](Layout &l) {
        l.type = MTLTextureType3D;
        l.depth = 4;
        l.size = {32, 32, 4};
        l.image = 32 * 4 * 32 + 256;
    });

    struct Format {
        const char *name;
        MTLPixelFormat format;
        NSUInteger bytes;
    };

    for (const Format f : {Format{"r8", MTLPixelFormatR8Unorm, 1},
                           Format{"rg8", MTLPixelFormatRG8Unorm, 2},
                           Format{"bgra8", MTLPixelFormatBGRA8Unorm, 4},
                           Format{"srgb", MTLPixelFormatRGBA8Unorm_sRGB, 4},
                           Format{"r16f", MTLPixelFormatR16Float, 2},
                           Format{"rgba16f", MTLPixelFormatRGBA16Float, 8},
                           Format{"r32uint", MTLPixelFormatR32Uint, 4},
                           Format{"r32f", MTLPixelFormatR32Float, 4},
                           Format{"rg32f", MTLPixelFormatRG32Float, 8},
                           Format{"rgba32f", MTLPixelFormatRGBA32Float, 16},
                           Format{"rgb10a2", MTLPixelFormatRGB10A2Unorm, 4},
                           Format{"bgr10a2", MTLPixelFormatBGR10A2Unorm, 4},
                           Format{"rg11b10f", MTLPixelFormatRG11B10Float, 4},
                           Format{"rgb9e5", MTLPixelFormatRGB9E5Float, 4},
                           Format{"b5g6r5", MTLPixelFormatB5G6R5Unorm, 2},
                           Format{"abgr4", MTLPixelFormatABGR4Unorm, 2},
                           Format{"rgba8uint", MTLPixelFormatRGBA8Uint, 4},
                           Format{"rg16uint", MTLPixelFormatRG16Uint, 4},
                           Format{"rgba16uint", MTLPixelFormatRGBA16Uint, 8},
                           Format{"rgba16", MTLPixelFormatRGBA16Unorm, 8}})
        add(f.name, [&](Layout &l) {
            l.format = f.format;
            l.texelBytes = f.bytes;
        });
    add("astc 4x4", [](Layout &l) {
        l.format = MTLPixelFormatASTC_4x4_LDR;
        l.texelBytes = 16;
        l.block = 4;
    });
    add("astc 8x8 sub-rect", [](Layout &l) {
        l.format = MTLPixelFormatASTC_8x8_LDR;
        l.texelBytes = 16;
        l.block = 8;
        l.origin = {8, 16, 0};
        l.size = {16, 8, 1};
    });
    add("etc2 rgb8", [](Layout &l) {
        l.format = MTLPixelFormatETC2_RGB8;
        l.texelBytes = 8;
        l.block = 4;
    });
    add("depth32f", [](Layout &l) {
        l.target = true;
        l.format = MTLPixelFormatDepth32Float;
    });
    add("depth16", [](Layout &l) {
        l.target = true;
        l.format = MTLPixelFormatDepth16Unorm;
        l.texelBytes = 2;
    });
    add("stencil8", [](Layout &l) {
        l.target = true;
        l.format = MTLPixelFormatStencil8;
        l.texelBytes = 1;
    });
    add("depth of depth32f_stencil8", [](Layout &l) {
        l.target = true;
        l.format = MTLPixelFormatDepth32Float_Stencil8;
        l.options = MTLBlitOptionDepthFromDepthStencil;
    });
    add("stencil of depth32f_stencil8", [](Layout &l) {
        l.target = true;
        l.format = MTLPixelFormatDepth32Float_Stencil8;
        l.options = MTLBlitOptionStencilFromDepthStencil;
        l.texelBytes = 1;
    });
    add("4x msaa", [](Layout &l) {
        l.type = MTLTextureType2DMultisample;
        l.samples = 4;
        l.metalRefuses = true;
    });
    return out;
}

// Each layout's copies on the textures the driver holds; the rest listed
// RAW-31: a one-level 2D texture of shared storage in memory of its own is block-linear, as a private one is; one over a
// buffer stays pitch (the buffer's binding names its rows)
TEST_F(FacadeCopies, ASharedTextureOfItsOwnMemoryIsBlockLinear)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> shared = [gDevice newTextureWithDescriptor:d];
    ASSERT_TRUE([(id)shared isKindOfClass:[SwitchMetalTexture class]]);
    const SwitchMetalTexture *t = (SwitchMetalTexture *)shared;
    ASSERT_TRUE(t->_gm20bOk);
    EXPECT_TRUE(t->_raw.blockLinear);
    EXPECT_FALSE(t->_raw.wc);
    id<MTLBuffer> rows = [gDevice newBufferWithLength:256 * 64 options:MTLResourceStorageModeShared];
    id<MTLTexture> over = [rows newTextureWithDescriptor:d offset:0 bytesPerRow:256];
    ASSERT_TRUE([(id)over isKindOfClass:[SwitchMetalTexture class]]);
    EXPECT_FALSE(((SwitchMetalTexture *)over)->_raw.blockLinear);
}

TEST_F(FacadeCopies, EveryLayoutMetalTakesOnATextureTheRawPathHoldsIsItsToo)
{
    std::vector<std::string> refused, held, notHeld;
    for (const Layout &l : layouts()) {
        const std::string r = refusedDirections(l);
        if (r == kNotHeld) {
            notHeld.push_back(l.name);
            continue;
        }
        held.push_back(l.name);
        if (l.metalRefuses ? r != " b2t@skipped t2b@skipped" : !r.empty())
            refused.push_back(l.name + ":" + (r.empty() ? " taken" : r));
    }
    std::string list, unheld;
    for (const std::string &r : refused)
        list += "\n  " + r;
    for (const std::string &n : notHeld)
        unheld += " [" + n + "]";
    printf("textures the driver does not hold:%s\n", unheld.c_str());
    EXPECT_TRUE(refused.empty()) << refused.size() << " layouts answered unlike Metal:" << list;
    EXPECT_GE(held.size(), 30u);
    for (const std::string &n : notHeld)
        EXPECT_TRUE(n.find("depth") == std::string::npos && n.find("stencil") == std::string::npos)
            << n << ": a stored depth/stencil texture is the driver's own";
    for (const char *formerlyM2v : {"etc2 rgb8", "array slice 1 level 1", "r32uint", "rgba16uint"})
        EXPECT_TRUE(std::find(notHeld.begin(), notHeld.end(), formerlyM2v) == notHeld.end())
            << formerlyM2v << ": not held, so its copies are m2v's";
}

// An R8Unorm `width` x `height` over the luma plane of a '420v' whose rows are `pitch` bytes apart, longer than they
// are: its rows overlap (an M4 Max reads and writes row y at the plane's byte y * pitch: plain_rows.h), every byte of
// the surface seeded
struct OverlappingRows {
    IOSurfaceRef surface = nullptr;
    id<MTLTexture> texture = nil;

    OverlappingRows(NSUInteger width, NSUInteger height, NSUInteger pitch)
    {
        const NSUInteger chroma = (pitch * height + 16383) & ~NSUInteger(16383);
        NSDictionary *luma = @{
            (id)kIOSurfacePlaneWidth : @(pitch),
            (id)kIOSurfacePlaneHeight : @(height),
            (id)kIOSurfacePlaneBytesPerElement : @1,
            (id)kIOSurfacePlaneBytesPerRow : @(pitch),
            (id)kIOSurfacePlaneOffset : @0,
            (id)kIOSurfacePlaneSize : @(pitch * height)
        };
        NSDictionary *pairs = @{
            (id)kIOSurfacePlaneWidth : @1,
            (id)kIOSurfacePlaneHeight : @1,
            (id)kIOSurfacePlaneBytesPerElement : @2,
            (id)kIOSurfacePlaneBytesPerRow : @16,
            (id)kIOSurfacePlaneOffset : @(chroma),
            (id)kIOSurfacePlaneSize : @16
        };
        surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
            (id)kIOSurfaceWidth : @(pitch),
            (id)kIOSurfaceHeight : @(height),
            (id)kIOSurfacePixelFormat : @(uint32_t('420v')),
            (id)kIOSurfaceAllocSize : @(chroma + 16384),
            (id)kIOSurfacePlaneInfo : @[ luma, pairs ]
        });
        if (!surface)
            return;
        auto *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(surface));
        for (size_t i = 0; i < IOSurfaceGetAllocSize(surface); ++i)
            base[i] = uint8_t(i * 37 + (i >> 7));
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        texture = [gDevice newTextureWithDescriptor:d iosurface:surface plane:0];
    }

    ~OverlappingRows()
    {
        if (surface)
            CFRelease(surface);
    }

    const uint8_t *bytes() const
    {
        return static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(surface));
    }
};

// gpu.gm20b.plain_rows_reads' overlapping arm, R8Unorm 136x104 over rows of 16 bytes: a copy of it into a buffer moves
// row y from the plane's byte y * 16, as every read of it does. The copy engine's model holds a launch of lines longer
// than a pitch side's rows, which never completes on the GM20B (boot 1: CTXSW_TIMEOUT with CE2 busy in a context save)
TEST_F(FacadeCopies, ACopyOutOfRowsThatOverlapReadsEachRowAtThePlanesPitch)
{
    constexpr NSUInteger kWidth = 136, kHeight = 104, kPitch = 16;
    OverlappingRows rows(kWidth, kHeight, kPitch);
    ASSERT_TRUE(rows.texture != nil && ((SwitchMetalTexture *)rows.texture)->_gm20bOk);
    ASSERT_EQ(((SwitchMetalTexture *)rows.texture)->_raw.pitch, kPitch);
    const std::vector<uint8_t> surface(rows.bytes(), rows.bytes() + IOSurfaceGetAllocSize(rows.surface));
    id<MTLBuffer> out = [gDevice newBufferWithLength:kWidth * kHeight options:MTLResourceStorageModeShared];
    memset(out.contents, 0xee, out.length);
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:rows.texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(kWidth, kHeight, 1)
                        toBuffer:out
               destinationOffset:0
          destinationBytesPerRow:kWidth
        destinationBytesPerImage:kWidth * kHeight];
    [e endEncoding];
    EngineRun run;
    ASSERT_NO_FATAL_FAILURE(commitOnRaw(cb, run));
    const auto *got = static_cast<const uint8_t *>(out.contents);
    size_t wrong = 0;
    for (NSUInteger y = 0; y < kHeight; ++y)
        for (NSUInteger x = 0; x < kWidth; ++x)
            wrong += got[y * kWidth + x] != surface[y * kPitch + x];
    EXPECT_EQ(wrong, 0u) << "of " << kWidth * kHeight;
    EXPECT_EQ(std::vector<uint8_t>(rows.bytes(), rows.bytes() + surface.size()), surface) << "the plane is only read";
}

// A copy from a buffer into R8Unorm 136x40 over rows of 64 bytes (gpu.gm20b.plain_rows_cpu's shape) lands its rows in
// row order: each plane byte two or more rows reach holds the last such row's byte, every byte past the rows as it was
TEST_F(FacadeCopies, ACopyIntoRowsThatOverlapLandsTheLaterRowsBytes)
{
    constexpr NSUInteger kWidth = 136, kHeight = 40, kPitch = 64;
    OverlappingRows rows(kWidth, kHeight, kPitch);
    ASSERT_TRUE(rows.texture != nil && ((SwitchMetalTexture *)rows.texture)->_gm20bOk);
    std::vector<uint8_t> want(rows.bytes(), rows.bytes() + IOSurfaceGetAllocSize(rows.surface));
    id<MTLBuffer> in = [gDevice newBufferWithLength:kWidth * kHeight options:MTLResourceStorageModeShared];
    auto *codes = static_cast<uint8_t *>(in.contents);
    for (NSUInteger i = 0; i < in.length; ++i)
        codes[i] = uint8_t(i / kWidth * 5 + i % kWidth * 3 + 1);
    for (NSUInteger y = 0; y < kHeight; ++y)
        for (NSUInteger x = 0; x < kWidth; ++x)
            want[y * kPitch + x] = codes[y * kWidth + x];
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:in
               sourceOffset:0
          sourceBytesPerRow:kWidth
        sourceBytesPerImage:kWidth * kHeight
                 sourceSize:MTLSizeMake(kWidth, kHeight, 1)
                  toTexture:rows.texture
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e endEncoding];
    EngineRun run;
    ASSERT_NO_FATAL_FAILURE(commitOnRaw(cb, run));
    size_t wrong = 0;
    for (size_t i = 0; i < want.size(); ++i)
        wrong += rows.bytes()[i] != want[i];
    EXPECT_EQ(wrong, 0u) << "of " << want.size() << " bytes of the surface";
}

}  // namespace
