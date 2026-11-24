// SPDX-License-Identifier: MIT
// private_comp.h: which private textures bind compressed memory, and of which kind.
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/private_comp.h"
#include "drivers/TegraGM20BMetal/storage_form.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

gm20b::PrivateTextureFacts target(uint32_t bpp)
{
    gm20b::PrivateTextureFacts f;
    f.privateStorage = f.compressibleUsage = f.plainColour = true;
    f.shape = gm20b::PrivateShape2D::Plain;
    f.compression = true;
    f.bpp = bpp;
    return f;
}

TEST(GM20BPrivateComp, TargetsTakeTheColourKindsTheCopyEngineReads)
{
    EXPECT_EQ(gm20b::privateCompressedKind(target(4)), uint32_t(gm20b::kCompKindC32));
    EXPECT_EQ(gm20b::privateCompressedKind(target(8)), uint32_t(gm20b::kCompKindC64));
    EXPECT_EQ(gm20b::privateCompressedKind(target(16)), uint32_t(gm20b::kCompKindC128)) << "RGBA32Float";
    EXPECT_EQ(gm20b::privateCompressedKind(target(2)), 0u);
}

TEST(GM20BPrivateComp, AnythingElseStaysPlain)
{
    bool gm20b::PrivateTextureFacts::*fields[] = {
        &gm20b::PrivateTextureFacts::privateStorage,
        &gm20b::PrivateTextureFacts::compressibleUsage,
        &gm20b::PrivateTextureFacts::plainColour,
        &gm20b::PrivateTextureFacts::compression,
    };
    for (auto field : fields) {
        gm20b::PrivateTextureFacts f = target(4);
        f.*field = false;
        EXPECT_EQ(gm20b::privateCompressedKind(f), 0u);
    }
    gm20b::PrivateTextureFacts other = target(4);
    other.shape = gm20b::PrivateShape2D::Other;
    EXPECT_EQ(gm20b::privateCompressedKind(other), 0u) << "a cube, a 3D or a 1D texture";
}

TEST(GM20BPrivateComp, ShaderWriteUsageIsTakenWithStorageReadingCompressed)
{
    const std::string facade = Body(SourceText("../TegraGM20BMetalPrivateComp.mm"), "uint32_t privateCompressedKind(");
    EXPECT_THAT(facade,
                HasSubstr("MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite"))
        << "gpu.gm20b.private_compression shader_write";
    EXPECT_TRUE(gm20b::storageReadsCompressed());
}

TEST(GM20BPrivateComp, AMipChainTakesItsKind)
{
    gm20b::PrivateTextureFacts f = target(4);
    f.levels = 5;
    EXPECT_EQ(gm20b::privateCompressedKind(f), uint32_t(gm20b::kCompKindC32)) << "gpu.gm20b.private_compression mips";
}

TEST(GM20BPrivateComp, AOneLevelArrayTakesItsKind)
{
    gm20b::PrivateTextureFacts f = target(8);
    f.shape = gm20b::PrivateShape2D::Array;
    EXPECT_EQ(gm20b::privateCompressedKind(f), uint32_t(gm20b::kCompKindC64)) << "gpu.gm20b.private_compression array";
    f.levels = 2;
    EXPECT_EQ(gm20b::privateCompressedKind(f), 0u) << "an array's levels are no chain the move copies";
}

// A stored multisampled target takes its sample count's kind where the copy engine reads that kind (the move onto
// compressed memory is its copy of the sample grid)
TEST(GM20BPrivateComp, AStoredMultisampledTargetTakesItsKindWhereTheCopyEngineReadsIt)
{
    gm20b::PrivateTextureFacts f = target(4);
    f.shape = gm20b::PrivateShape2D::Multisample;
    f.samples = 4;
    const uint32_t kind = gm20b::compressedKind(4, 0, 4);
    EXPECT_EQ(gm20b::privateCompressedKind(f), gm20b::copyEngineReadsCompressed(kind) ? kind : 0u);
    EXPECT_TRUE(gm20b::privateShapeCompresses(f)) << "gpu.gm20b.private_compression ms4";
    f.samples = 8;
    EXPECT_FALSE(gm20b::privateShapeCompresses(f));
}

// The copy engine moves a multisampled texture as fx::msLayout's grid of its samples
TEST(GM20BPrivateComp, AMultisampledTextureMovesAsItsSampleGrid)
{
    uint32_t sx = 9, sy = 9;
    gm20b::sampleGrid(1, sx, sy);
    EXPECT_EQ(sx, 0u);
    EXPECT_EQ(sy, 0u);
    gm20b::sampleGrid(2, sx, sy);
    EXPECT_EQ(sx, 1u);
    EXPECT_EQ(sy, 0u);
    gm20b::sampleGrid(4, sx, sy);
    EXPECT_EQ(sx, 1u);
    EXPECT_EQ(sy, 1u);
}

// A private depth texture of one sample, layer and level takes its stored zeta kind (comp.h storedZetaKind), which
// its want asks for where none was ready at its creation
TEST(GM20BPrivateComp, APrivateDepthTextureTakesItsStoredZetaKind)
{
    gm20b::PrivateTextureFacts f;
    f.privateStorage = f.compressibleUsage = f.compression = true;
    f.shape = gm20b::PrivateShape2D::Plain;
    f.zeta = 252;
    EXPECT_EQ(gm20b::privateCompressedKind(f), uint32_t(gm20b::kCompKindZF32));
    f.zeta = 260;
    EXPECT_EQ(gm20b::privateCompressedKind(f), uint32_t(gm20b::kCompKindZeta));
    f.zeta = 250;
    EXPECT_EQ(gm20b::privateCompressedKind(f), uint32_t(gm20b::kCompKindZ16));
    f.zeta = 253;
    EXPECT_EQ(gm20b::privateCompressedKind(f), 0u) << "Stencil8 has no compressible kind";
    f.zeta = 252;
    f.layers = 6;
    EXPECT_EQ(gm20b::privateCompressedKind(f), 0u) << "a cube";
    f.layers = 1;
    f.levels = 2;
    EXPECT_EQ(gm20b::privateCompressedKind(f), 0u);
    f.levels = 1;
    f.samples = 4;
    EXPECT_EQ(gm20b::privateCompressedKind(f), 0u) << "a multisampled one stays plain";
    f.samples = 1;
    f.privateStorage = false;
    EXPECT_EQ(gm20b::privateCompressedKind(f), 0u) << "a shared one: no want, as a CPU copy may reach it";
}

TEST(GM20BPrivateComp, EveryLaidOutShapeTakesCompressedMemoryAtItsCreation)
{
    const std::string laid = Body(SourceText("../textures.cpp"), "bool Device::createLaidOut(");
    EXPECT_THAT(laid, HasSubstr("ask.compressKind && !at && !zeta && takeCompressed(bytes, ask.compressKind, comp)"));
    EXPECT_THAT(laid, HasSubstr("noteCompressed(ask, comp, bytes, t);"));
}

TEST(GM20BPrivateComp, AStoredZetaTakesCompressedMemoryInEveryBuild)
{
    const std::string zeta = Body(SourceText("../multisample.cpp"), "bool Device::createOwnedZeta(");
    EXPECT_THAT(zeta, HasSubstr("storedZetaKind("));
    EXPECT_THAT(zeta, HasSubstr("createCompressedZeta(bytes, kind, t)"));
    EXPECT_THAT(zeta, Not(HasSubstr("#if"))) << "the compressed route is the release one, not a probe's";
    const std::string made = Body(SourceText("../private_comp.cpp"), "bool Device::createCompressedZeta(");
    EXPECT_THAT(made, HasSubstr("takeCompressed(bytes, kind, bo)")) << "memory made off the render thread, as colour's";
    EXPECT_THAT(Body(SourceText("../private_comp.cpp"), "bool Device::takeCompressed("),
                HasSubstr("comp_.fit(need, kind)"));
    EXPECT_THAT(made, Not(HasSubstr("allocCompressible")));
    EXPECT_THAT(Body(SourceText("../gpu.h"), "    void freeOwnedZeta(uint64_t va, DeferredFrees &deferred)"),
                HasSubstr("freeCompressedZeta(va, deferred);"));
}

}  // namespace
