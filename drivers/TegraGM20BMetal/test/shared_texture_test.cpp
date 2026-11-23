// SPDX-License-Identifier: MIT
// shared_texture.h, surface_image.h and TegraGM20BMetalSharedTexture.h, every kind: a shared texture's
// IOSurface creation properties from its descriptor and the descriptor read back from them. The keys are those an M4
// Max's Metal writes for the same descriptor (its newSharedTextureWithDescriptor:, macOS 27), its swizzle key that
// Metal's own. Any shape but a one-level 2D one is laid out in the surface as the driver lays the descriptor out in
// memory of its own; the AGX drivers' surface of it holds AllocSize and kAGXCompressionMode alone.
#include "../TegraGM20BMetalSharedTexture.h"
#include "../surface_image.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kRegistry = 0x10000065b;

NSArray<NSString *> *metalKeys(const gm20b::shared::Kind &k)
{
    NSMutableArray *keys = [@[
        @"kMetalRegistryID", @"kMetalTextureType", @"kMetalTexturePixelFormat", @"kMetalTextureWidth",
        @"kMetalTextureHeight", @"kMetalTextureDepth", @"kMetalTextureMipmapLevelCount", @"kMetalTextureSampleCount",
        @"kMetalTextureArrayLength", @"kMetalTextureResourceOptions", @"kMetalTextureUsage",
        @"kMetalTextureSparseValue", @"kMetalTextureGPUOptimization", @"kMetalTextureSwizzleKey",
        @"kMetalTextureRotation", @"kMetalTextureFootprint", @"kMetalTextureResourceIndex", @"kMetalTextureCSCMatrix"
    ] mutableCopy];
    if (k.writeAccessPattern)
        [keys addObject:@"kMetalTextureWriteAccessPattern"];
    return [keys sortedArrayUsingSelector:@selector(compare:)];
}

MTLTextureDescriptor *descriptor()
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:100
                                                                                height:70
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    d.allowGPUOptimizedContents = NO;
    d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed,
                                              MTLTextureSwizzleAlpha);
    return d;
}

NSMutableDictionary *properties(MTLTextureDescriptor *d, const gm20b::shared::Kind &k)
{
    NSMutableDictionary *p = gm20bSharedSurfaceProperties(d, 4);
    gm20bAddSharedTextureKeys(p, d, kRegistry, k);
    return p;
}

uint64_t number(NSDictionary *p, id key)
{
    return [p[key] unsignedLongLongValue];
}

TEST(GM20BSharedTexture, SurfaceRowsAndRoomForTheBlockLinearLayout)
{
    gm20b::shared::SurfaceShape s;
    ASSERT_TRUE(gm20b::shared::surfaceShape(100, 70, 4, s));
    EXPECT_EQ(s.bytesPerRow, 512u);
    EXPECT_EQ(s.allocBytes, 57344u);  // NIL's 448-byte rows, 128 of them in 64-row blocks, over 70 rows of 512
    ASSERT_TRUE(gm20b::shared::surfaceShape(64, 48, 4, s));
    EXPECT_EQ(s.bytesPerRow, 256u);
    EXPECT_EQ(s.allocBytes, 16384u);
    ASSERT_TRUE(gm20b::shared::surfaceShape(10, 10, 12, s));  // a texel size NIL does not tile: the rows alone
    EXPECT_EQ(s.bytesPerRow, 128u);
    EXPECT_EQ(s.allocBytes, 1280u);
    EXPECT_FALSE(gm20b::shared::surfaceShape(0, 8, 4, s));
    EXPECT_FALSE(gm20b::shared::surfaceShape(16385, 8, 4, s));
    EXPECT_EQ(gm20bSharedSurfaceProperties(descriptor(), 0), nil);
}

TEST(GM20BSharedTexture, CreationPropertiesHoldTheDescriptorUnderMetalsKeys)
{
    for (const gm20b::shared::Kind &k : gm20b::shared::kKinds) {
        NSDictionary *p = properties(descriptor(), k);
        NSArray *keys =
            [[p.allKeys filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'kMetal'"]]
                sortedArrayUsingSelector:@selector(compare:)];
        EXPECT_TRUE([keys isEqualToArray:metalKeys(k)]) << k.kind << ": " << keys.description.UTF8String;
        EXPECT_EQ(number(p, @"kMetalRegistryID"), kRegistry);
        EXPECT_EQ(number(p, @"kMetalTextureType"), MTLTextureType2D);
        EXPECT_EQ(number(p, @"kMetalTexturePixelFormat"), MTLPixelFormatRGBA8Unorm);
        EXPECT_EQ(number(p, @"kMetalTextureWidth"), 100u);
        EXPECT_EQ(number(p, @"kMetalTextureHeight"), 70u);
        EXPECT_EQ(number(p, @"kMetalTextureDepth"), 1u);
        EXPECT_EQ(number(p, @"kMetalTextureMipmapLevelCount"), 1u);
        EXPECT_EQ(number(p, @"kMetalTextureSampleCount"), 1u);
        EXPECT_EQ(number(p, @"kMetalTextureArrayLength"), 1u);
        EXPECT_EQ(number(p, @"kMetalTextureResourceOptions"), MTLResourceStorageModePrivate);
        EXPECT_EQ(number(p, @"kMetalTextureUsage"), MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        EXPECT_EQ(number(p, @"kMetalTextureSwizzleKey"), 0x05020304u);
        EXPECT_EQ(CFGetTypeID((__bridge CFTypeRef)p[@"kMetalTextureGPUOptimization"]), CFBooleanGetTypeID());
        EXPECT_EQ(number(p, @"kMetalTextureGPUOptimization"), 0u);
        EXPECT_EQ(number(p, @"IOSurfaceProtectionOptions"), 0u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceAllocSize), 57344u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceBytesPerRow), 512u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceBytesPerElement), 4u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceWidth), 100u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceHeight), 70u);
    }
}

TEST(GM20BSharedTexture, TheDescriptorComesBackFromTheProperties)
{
    for (const gm20b::shared::Kind &k : gm20b::shared::kKinds) {
        MTLTextureDescriptor *d = descriptor();
        const bool sparse = [d respondsToSelector:gm20bSharedSetter("sparseSurfaceDefaultValue")];
        if (sparse)
            [d setValue:@1 forKey:@"sparseSurfaceDefaultValue"];
        MTLTextureDescriptor *back = gm20bSharedTextureDescriptor(properties(d, k), kRegistry, k);
        ASSERT_NE(back, nil) << k.kind;
        EXPECT_EQ(back.textureType, d.textureType);
        EXPECT_EQ(back.pixelFormat, d.pixelFormat);
        EXPECT_EQ(back.width, d.width);
        EXPECT_EQ(back.height, d.height);
        EXPECT_EQ(back.depth, d.depth);
        EXPECT_EQ(back.mipmapLevelCount, d.mipmapLevelCount);
        EXPECT_EQ(back.sampleCount, d.sampleCount);
        EXPECT_EQ(back.arrayLength, d.arrayLength);
        EXPECT_EQ(back.resourceOptions, d.resourceOptions);
        EXPECT_EQ(back.storageMode, MTLStorageModePrivate);
        EXPECT_EQ(back.cpuCacheMode, d.cpuCacheMode);
        EXPECT_EQ(back.hazardTrackingMode, d.hazardTrackingMode);
        EXPECT_EQ(back.usage, d.usage);
        EXPECT_EQ(back.allowGPUOptimizedContents, NO);
        EXPECT_EQ(gm20bSwizzleKey(back.swizzle), gm20bSwizzleKey(d.swizzle));
        if (sparse)
            EXPECT_EQ([[back valueForKey:@"sparseSurfaceDefaultValue"] unsignedIntegerValue], 1u);
    }
}

TEST(GM20BSharedTexture, AnotherDevicesOrAnIncompleteSurfaceOpensNothing)
{
    for (const gm20b::shared::Kind &k : gm20b::shared::kKinds) {
        NSDictionary *p = properties(descriptor(), k);
        EXPECT_EQ(gm20bSharedTextureDescriptor(p, kRegistry + 1, k), nil) << k.kind;
        EXPECT_EQ(gm20bSharedTextureDescriptor(p, 0, k), nil) << k.kind;
        for (NSString *key in metalKeys(k)) {
            NSMutableDictionary *missing = [p mutableCopy];
            [missing removeObjectForKey:key];
            EXPECT_EQ(gm20bSharedTextureDescriptor(missing, kRegistry, k), nil)
                << k.kind << " without " << key.UTF8String;
        }
        NSMutableDictionary *wrong = [p mutableCopy];
        wrong[@"kMetalTextureWidth"] = @"100";
        EXPECT_EQ(gm20bSharedTextureDescriptor(wrong, kRegistry, k), nil) << k.kind;
    }
    const gm20b::shared::Kind &k17 = gm20b::shared::kKinds[0], &k27 = gm20b::shared::kKinds[1];
    ASSERT_FALSE(k17.writeAccessPattern);
    ASSERT_TRUE(k27.writeAccessPattern);
    EXPECT_EQ(gm20bSharedTextureDescriptor(properties(descriptor(), k17), kRegistry, k27), nil);
    EXPECT_NE(gm20bSharedTextureDescriptor(properties(descriptor(), k27), kRegistry, k17), nil);
}

gm20b::SurfaceImageFacts rgba8(uint32_t type, uint32_t width, uint32_t height)
{
    gm20b::SurfaceImageFacts d;
    d.type = type;
    d.width = width;
    d.height = height;
    d.bpp = 4;
    d.target = true;
    return d;
}

gm20b::SurfaceImage laid(const gm20b::SurfaceImageFacts &d)
{
    gm20b::SurfaceImage image;
    const char *why = gm20b::surfaceImage(d, image);
    EXPECT_EQ(why, nullptr) << (why ? why : "");
    return image;
}

bool sameLayout(const gm20b::fx::MipChain &a, const gm20b::fx::MipChain &b)
{
    return a.bytes == b.bytes && a.levels == b.levels && a.layers == b.layers && a.arrayStride == b.arrayStride &&
           a.depth == b.depth && a.zLog2 == b.zLog2 && a.samples == b.samples && a.yLog2 == b.yLog2 &&
           a.level[0].rowBytes == b.level[0].rowBytes && a.level[a.levels - 1].offset == b.level[b.levels - 1].offset;
}

TEST(GM20BSharedTexture, EveryShapeIsTheImageTheRawPathLaysOut)
{
    EXPECT_EQ(laid(rgba8(gm20b::kType2D, 100, 70)).kind, gm20b::SurfaceImage::Plain);
    gm20b::fx::MipChain c;

    gm20b::SurfaceImageFacts d = rgba8(gm20b::kType2D, 64, 48);
    d.levels = 7;
    gm20b::SurfaceImage image = laid(d);
    EXPECT_EQ(image.kind, gm20b::SurfaceImage::Chain);
    ASSERT_TRUE(gm20b::fx::mipChainLayout(64, 48, 7, 4, c));
    EXPECT_TRUE(sameLayout(image.layout, c));

    d = rgba8(gm20b::kType2DArray, 40, 24);
    d.arrayLength = 3;
    image = laid(d);
    EXPECT_EQ(image.kind, gm20b::SurfaceImage::Layers);
    EXPECT_EQ(image.type, gm20b::fx::kTexTwoDArray);
    ASSERT_TRUE(gm20b::fx::arrayLayout(40, 24, 3, 1, 4, c));
    EXPECT_TRUE(sameLayout(image.layout, c));

    image = laid(rgba8(gm20b::kTypeCube, 32, 32));
    EXPECT_EQ(image.type, gm20b::fx::kTexCube);
    EXPECT_EQ(image.layout.layers, 6u);
    d = rgba8(gm20b::kTypeCubeArray, 16, 16);
    d.arrayLength = 2;
    image = laid(d);
    EXPECT_EQ(image.type, gm20b::fx::kTexCubeArray);
    EXPECT_EQ(image.layout.layers, 12u);
    d = rgba8(gm20b::kType1DArray, 100, 1);
    d.arrayLength = 4;
    image = laid(d);
    EXPECT_EQ(image.type, gm20b::fx::kTexOneDArray);
    EXPECT_EQ(image.layout.layers, 4u);

    for (const bool rendered : {false, true}) {
        d = rgba8(gm20b::kType3D, 20, 12);
        d.depth = 9;
        d.rendered = rendered;
        image = laid(d);
        EXPECT_EQ(image.kind, gm20b::SurfaceImage::Volume);
        EXPECT_EQ(image.type, gm20b::fx::kTexThreeD);
        ASSERT_TRUE(gm20b::fx::volumeLayout(20, 12, 9, 1, 4, rendered, c));
        EXPECT_TRUE(sameLayout(image.layout, c));
    }

    d = rgba8(gm20b::kType2DMultisample, 30, 18);
    d.samples = 4;
    image = laid(d);
    EXPECT_EQ(image.kind, gm20b::SurfaceImage::Samples);
    ASSERT_TRUE(gm20b::fx::msLayout(30, 18, 4, 4, c));
    EXPECT_TRUE(sameLayout(image.layout, c));

    image = laid(rgba8(gm20b::kType1D, 300, 1));
    EXPECT_EQ(image.kind, gm20b::SurfaceImage::Line);
    tgm_layout line;
    ASSERT_TRUE(tgm_layout_tiled(300, 1, 4, &line));
    EXPECT_EQ(image.layout.bytes, line.bytes);
    EXPECT_EQ(image.layout.level[0].rowBytes, line.row_bytes);

    for (const uint32_t levels : {1u, 7u})
        for (const uint32_t type : {gm20b::kType2D, gm20b::kType2DArray}) {
            d = rgba8(type, 100, 60);
            d.bpp = 16;
            d.blockW = d.blockH = 4;
            d.target = false;
            d.levels = levels;
            d.arrayLength = type == gm20b::kType2DArray ? 3 : 1;
            image = laid(d);
            EXPECT_EQ(image.kind, gm20b::SurfaceImage::Blocks);
            EXPECT_EQ(image.type, type == gm20b::kType2DArray ? gm20b::fx::kTexTwoDArray : gm20b::fx::kTexTwoD);
            ASSERT_TRUE(gm20b::fx::blockChainLayout(100, 60, levels, d.arrayLength, 4, 4, 16, c));
            EXPECT_TRUE(sameLayout(image.layout, c));
        }
}

TEST(GM20BTextureLayers, EveryTextureTypeHasMetalsSlices)
{
    const auto same = [](uint32_t ours, MTLTextureType metal) {
        return ours == uint32_t(metal);
    };
    EXPECT_TRUE(same(gm20b::kType1D, MTLTextureType1D) && same(gm20b::kType1DArray, MTLTextureType1DArray) &&
                same(gm20b::kType2D, MTLTextureType2D) && same(gm20b::kType2DArray, MTLTextureType2DArray) &&
                same(gm20b::kType2DMultisample, MTLTextureType2DMultisample) &&
                same(gm20b::kTypeCube, MTLTextureTypeCube) && same(gm20b::kTypeCubeArray, MTLTextureTypeCubeArray) &&
                same(gm20b::kType3D, MTLTextureType3D));

    const struct {
        uint32_t type, arrayLength, layers;
    } rows[] = {
        {gm20b::kType1D, 1, 1},
        {gm20b::kType1DArray, 5, 5},
        {gm20b::kType2D, 1, 1},
        {gm20b::kType2DArray, 5, 5},
        {gm20b::kType2DMultisample, 1, 1},
        {gm20b::kTypeCube, 1, 6},
        {gm20b::kTypeCubeArray, 3, 18},
        {gm20b::kType3D, 1, 1},
        {MTLTextureType2DMultisampleArray, 4, 1},
        {MTLTextureTypeTextureBuffer, 1, 1},
        {gm20b::kType2DArray, 0, 1},
        {gm20b::kTypeCubeArray, 0, 6},
    };

    for (const auto &r : rows)
        EXPECT_EQ(gm20b::textureLayers(r.type, r.arrayLength), r.layers) << r.type << " x" << r.arrayLength;
    gm20b::SurfaceImage image;
    gm20b::SurfaceImageFacts d = rgba8(gm20b::kType1DArray, 64, 1);
    d.arrayLength = 5;
    ASSERT_EQ(gm20b::surfaceImage(d, image), nullptr);
    EXPECT_EQ(image.layout.layers, 5u) << "a 1D array's layers are its slices, no cube's six";
    d = rgba8(gm20b::kTypeCubeArray, 16, 16);
    d.arrayLength = 3;
    ASSERT_EQ(gm20b::surfaceImage(d, image), nullptr);
    EXPECT_EQ(image.layout.layers, 18u);
}

TEST(GM20BSharedTexture, MipmappedArraysCubesAndVolumesAreTheirLevelsLayout)
{
    gm20b::SurfaceImage image;
    gm20b::fx::MipChain c;
    gm20b::SurfaceImageFacts d = rgba8(gm20b::kTypeCubeArray, 32, 32);
    d.arrayLength = 2;
    d.levels = 6;
    ASSERT_EQ(gm20b::surfaceImage(d, image), nullptr);
    EXPECT_EQ(image.kind, gm20b::SurfaceImage::Layers);
    ASSERT_TRUE(gm20b::fx::arrayLayout(32, 32, 12, 6, 4, c));
    EXPECT_TRUE(sameLayout(image.layout, c));
    d = rgba8(gm20b::kType3D, 20, 12);
    d.depth = 9;
    d.levels = 3;
    ASSERT_EQ(gm20b::surfaceImage(d, image), nullptr);
    EXPECT_EQ(image.kind, gm20b::SurfaceImage::Volume);
    ASSERT_TRUE(gm20b::fx::volumeLayout(20, 12, 9, 3, 4, false, c));
    EXPECT_TRUE(sameLayout(image.layout, c));
}

TEST(GM20BSharedTexture, ShapesTheRawPathKeepsInNoMemoryHaveNoImage)
{
    gm20b::SurfaceImage image;
    gm20b::SurfaceImageFacts d = rgba8(gm20b::kType2DArray, 32, 32);
    d.levels = 2;
    d.target = false;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "a mipmapped array, not a target";
    d = rgba8(gm20b::kType3D, 32, 32);
    d.levels = 2;
    d.target = false;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "a mipmapped 3D texture, not a target";
    d = rgba8(gm20b::kType2DMultisample, 32, 32);
    d.samples = 8;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "8 samples";
    d.samples = 4;
    d.target = false;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "multisampled, not a target";
    d = rgba8(gm20b::kType2D, 32, 32);
    d.levels = 3;
    d.target = false;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "mipmapped, not a target";
    d = rgba8(gm20b::kType2DArray, 32, 32);
    d.extended = true;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "an extended-range array";
    d = rgba8(gm20b::kTypeCube, 32, 16);
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "a cube of unequal sides";
    d = rgba8(gm20b::kType3D, 32, 32);
    d.blockW = d.blockH = 4;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "a block-compressed 3D texture";
    d = rgba8(gm20b::kType2D, 32, 32);
    d.bpp = 0;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "no header";
    d = rgba8(gm20b::kType2DArray, 0, 32);
    d.arrayLength = 2;
    EXPECT_NE(gm20b::surfaceImage(d, image), nullptr) << "no texels";
}

TEST(GM20BSharedTexture, ALaidOutSurfaceHoldsAGXsPropertiesAndIOSurfaceMakesIt)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:32 mipmapped:NO];
    d.textureType = MTLTextureTypeCubeArray;
    d.arrayLength = 2;
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    gm20b::SurfaceImageFacts facts = rgba8(gm20b::kTypeCubeArray, 32, 32);
    facts.arrayLength = 2;
    const gm20b::SurfaceImage image = laid(facts);
    for (const gm20b::shared::Kind &k : gm20b::shared::kKinds) {
        NSMutableDictionary *p = gm20bLaidOutSurfaceProperties(image.layout.bytes);
        EXPECT_EQ(p.count, 2u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceAllocSize), image.layout.bytes);
        ASSERT_NE(p[@"kAGXCompressionMode"], nil);
        EXPECT_EQ(number(p, @"kAGXCompressionMode"), 0u);
        gm20bAddSharedTextureKeys(p, d, kRegistry, k);
        MTLTextureDescriptor *back = gm20bSharedTextureDescriptor(p, kRegistry, k);
        ASSERT_NE(back, nil) << k.kind;
        EXPECT_EQ(back.textureType, MTLTextureTypeCubeArray);
        EXPECT_EQ(back.arrayLength, 2u);
        EXPECT_EQ(back.width, 32u);
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        ASSERT_NE(s, nullptr) << k.kind;
        EXPECT_GE(IOSurfaceGetAllocSize(s), image.layout.bytes);
        EXPECT_EQ(IOSurfaceGetPlaneCount(s), 0u);
        EXPECT_EQ(IOSurfaceGetWidth(s), 0u);
        CFRelease(s);
    }
    EXPECT_EQ(IOSurfaceCreate((__bridge CFDictionaryRef)gm20bLaidOutSurfaceProperties(0)), nullptr);
}

// _MTLDevice's own methods in each kind's ABI table (drivers/TegraGM20BMetal/abi/kinds)
TEST(GM20BSharedTexture, AResourceIndexOpensWhereTheKindsMetalHasIt)
{
    for (const gm20b::shared::Kind &k : gm20b::shared::kKinds) {
        NSString *path = [NSString
            stringWithFormat:@"drivers/TegraGM20BMetal/abi/kinds/%.*s.json", int(k.kind.size()), k.kind.data()];
        NSData *data = [NSData dataWithContentsOfFile:path];
        ASSERT_NE(data, nil) << path.UTF8String;
        NSDictionary *table = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        NSDictionary *methods = table[@"MTLDevice"][@"methods"];
        EXPECT_NE(methods[@"newSharedTextureWithHandle:"], nil) << k.kind;
        EXPECT_EQ(methods[@"newSharedTextureWithHandle:withResourceIndex:"] != nil, k.resourceIndexOpen) << k.kind;
    }
}

}  // namespace
