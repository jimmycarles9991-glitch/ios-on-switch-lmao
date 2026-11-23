// SPDX-License-Identifier: MIT
// Every descriptor shape that Metal, an identity and GM20B all make has a raw creator (texture_shape.h route), so
// newTextureWithDescriptor: never finds a shape with none: every format Metal describes, of every type and storage mode,
// at the level, sample and layer counts that tell the creators apart.
#include "../decoded.h"
#include "../gpu.h"
#include "../metal_formats.h"
#include "../texture_shape.h"
#include <gtest/gtest.h>
#include <sstream>
#include <vector>

namespace {

using gm20b::shapes::TextureShape;

std::string describe(const TextureShape &s)
{
    std::ostringstream o;
    o << "format " << s.format << " type " << s.type << " storage " << s.storage << " levels " << s.levels
      << " samples " << s.samples << " array " << s.array;
    return o.str();
}

TEST(TextureRoutes, EveryShapeMadeHasACreator)
{
    std::vector<std::string> unrouted;
    uint32_t made = 0;
    for (const gm20b::answers::Kind &kind : gm20b::answers::kKinds)
        for (const gm20b::metal_formats::Described &f : gm20b::metal_formats::kDescribed)
            for (uint32_t type = 0; type <= 9; type++)
                for (uint32_t storage : {gm20b::shapes::kShared, gm20b::shapes::kManaged, gm20b::shapes::kPrivate,
                                         gm20b::shapes::kMemoryless})
                    for (uint64_t levels : {1u, 2u})
                        for (uint64_t samples : {1u, 2u, 4u})
                            for (uint64_t array : {1u, 2u})
                                for (uint64_t usage :
                                     {uint64_t(0), uint64_t(gm20b::shapes::kUsageRead),
                                      uint64_t(gm20b::shapes::kUsageRead | gm20b::shapes::kUsageTarget)}) {
                                    TextureShape s;
                                    s.format = f.mtl;
                                    s.type = type;
                                    s.storage = storage;
                                    s.levels = levels;
                                    s.samples = samples;
                                    s.array = array;
                                    s.usage = usage;
                                    s.width = 16;
                                    s.height =
                                        type == gm20b::kType1D || type == gm20b::kType1DArray || type == 9 ? 1 : 16;
                                    s.depth = type == gm20b::kType3D ? 4 : 1;
                                    s.ownMemory = true;
                                    // a texture buffer of a header row is made over a buffer of the device's
                                    // (TegraGM20BMetalDevice.mm), X32_Stencil8 as its combined format's view
                                    if (gm20b::shapes::metalRefusal(s) || gm20b::shapes::identityRefusal(s, kind) ||
                                        gm20b::shapes::hardwareRefusal(s) || (type == 9 && gm20b::formatInfo(f.mtl)) ||
                                        f.mtl == 261)
                                        continue;
                                    made++;
                                    if (gm20b::shapes::route(s) == gm20b::shapes::Route::None && unrouted.size() < 16)
                                        unrouted.push_back(std::string(kind.kind) + " " + describe(s));
                                }
    printf("  %u shapes made, %zu with no creator\n", made, unrouted.size());
    EXPECT_GT(made, 0u);
    EXPECT_EQ(unrouted, std::vector<std::string>{});
}

// A one-level 3D render target of a format the GM20B renders none of is laid out one plane a block, as one it renders is:
// a pass into a plane renders its stand-in from and into that plane alone, with no copy of a block's planes around it
TEST(TextureRoutes, AVolumeTargetOfAStandInFormatIsLaidOutOnePlaneABlock)
{
    for (const uint32_t format : {11u /* R8Unorm_sRGB */, 41u /* A1BGR5Unorm */, 553u /* BGRA10_XR_sRGB */}) {
        TextureShape s;
        s.type = gm20b::kType3D;
        s.format = format;
        s.storage = gm20b::shapes::kPrivate;
        s.usage = gm20b::shapes::kUsageRead | gm20b::shapes::kUsageTarget;
        s.width = s.height = 32;
        s.depth = 4;
        EXPECT_TRUE(gm20b::shapes::volumeView2d(s)) << format;
        s.usage = gm20b::shapes::kUsageRead;
        EXPECT_FALSE(gm20b::shapes::volumeView2d(s)) << format << ": sampled alone";
    }
}

// A texture of a format decoded on upload (PVRTC, ASTC 4x2 and 8x4, ASTC HDR) is made as the private image of its stored
// format, laid out as a render target (its decode passes draw into it), whatever the storage mode asked: GM20B refuses none
TEST(TextureRoutes, ADecodedFormatsTextureIsItsStoredFormatsImage)
{
    for (const gm20b::DecodedFormat &f : gm20b::kDecodedFormats)
        for (uint32_t type : {gm20b::kType2D, gm20b::kType2DArray, gm20b::kTypeCube, gm20b::kType3D})
            for (uint32_t storage : {gm20b::shapes::kShared, gm20b::shapes::kPrivate})
                for (uint64_t levels : {1u, 3u}) {
                    TextureShape s;
                    s.format = f.format;
                    s.type = type;
                    s.storage = storage;
                    s.levels = levels;
                    s.width = s.height = 16;
                    s.depth = type == gm20b::kType3D ? 4 : 1;
                    s.array = type == gm20b::kType2DArray ? 3 : 1;
                    s.usage = gm20b::shapes::kUsageRead;
                    s.ownMemory = true;
                    if (gm20b::shapes::metalRefusal(s))
                        continue;
                    EXPECT_EQ(gm20b::shapes::hardwareRefusal(s), nullptr) << describe(s);
                    const TextureShape held = gm20b::shapes::heldShape(s);
                    EXPECT_EQ(held.format, f.stored) << describe(s);
                    EXPECT_EQ(held.storage, gm20b::shapes::kPrivate) << describe(s);
                    EXPECT_NE(held.usage & gm20b::shapes::kUsageTarget, 0u) << describe(s);
                    EXPECT_NE(gm20b::shapes::route(s), gm20b::shapes::Route::None) << describe(s);
                    EXPECT_EQ(gm20b::shapes::route(s), gm20b::shapes::route(held)) << describe(s);
                }
}

// Metal makes no texture buffer of a block-compressed format: an M4 Max's validation stops BC1, PVRTC and ASTC alike
TEST(TextureRoutes, ABlockCompressedFormatTakesNoTextureBuffer)
{
    for (const uint32_t format : {130u, 166u, 203u, 204u, 222u}) {
        TextureShape s;
        s.format = format;
        s.type = 9;
        s.width = 64;
        EXPECT_NE(gm20b::shapes::metalRefusal(s), nullptr) << format;
    }
}

// MTLPixelFormatUnspecialized names no texture's format (an M4 Max; both identities answer it as Invalid)
TEST(TextureRoutes, UnspecializedIsNoTexturesFormat)
{
    TextureShape s;
    s.format = 263;
    EXPECT_NE(gm20b::shapes::metalRefusal(s), nullptr);
    EXPECT_NE(gm20b::shapes::formatRefusal(263), nullptr);
    EXPECT_EQ(gm20b::shapes::formatRefusal(53), nullptr) << "R32Uint";
}

}  // namespace
