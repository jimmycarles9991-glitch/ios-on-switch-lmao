// SPDX-License-Identifier: MIT
// kernel_texture.h: how a raw kernel reaches each texture it binds. metal.float32_filtering's
// three-level R32F, RG32F and RGBA32F textures went to m2v (compute.image) because a mipmapped texture was taken for
// one sampled through an image of its own; its header is its chain's, every level in place. mediaanalysisd's Core Image
// kernel bound an RGBA8 texture whose rows are 16 bytes off 32 (boot 21: 326 command buffers on m2v): it is reached
// through its shadow image, not refused.
#include <gtest/gtest.h>
#include <vector>

#include "../kernel_texture.h"

namespace {

using gm20b::KernelTextureForm;

struct Texture {
    unsigned tic = 7;
    uint32_t format = 0;
    uint8_t type = 0;
    bool planar = false, ycbcr = false, surfaceView = false, memoryless = false, xrPlanes = false;
    const Texture *standIn = nullptr, *sampled = nullptr;
    const void *chain = nullptr, *texelRows = nullptr;

    bool converted() const
    {
        return planar || ycbcr;
    }
};

// gpu.ca.glass_split's compute read of a 'b3a8' surface's RGB10A8_2P_XR10 texture: m2v compiles the kernel for the format
// it is asked at, and at 550 reads one R32G32_UINT image of the word and the alpha that no header of the two planes is,
// so admission refused it (compute.image b 2). Asked at its stand-in's RGBA16Float, the kernel's view is the stand-in's.
TEST(KernelTexture, ATwoPlaneTextureIsCompiledForAsItsStandIn)
{
    const Texture standIn;
    Texture t;
    t.xrPlanes = true;
    t.standIn = &standIn;
    EXPECT_EQ(gm20b::kernelTextureFormat(t, 550, true), gm20b::kXrStandInFormat);
    t.standIn = nullptr;
    EXPECT_EQ(gm20b::kernelTextureFormat(t, 550, false), 550u) << "no stand-in: refused as it was";
    EXPECT_EQ(gm20b::kernelTextureFormat(Texture(), 80, false), 80u);
}

// A kernel sampling a BGR10_XR texture through a mip filter, anisotropy or mixed min and mag filters has no form over
// the codes (the translator's NotImplemented(Format)): one a kernel does not read by its codes is read through its
// RGBA16Float stand-in, which the GM20B filters; one it does keeps its codes
TEST(KernelTexture, AnExtendedRangeTextureNoKernelReadsByItsCodesIsCompiledForAsItsStandIn)
{
    const Texture standIn;
    Texture codes;
    codes.standIn = &standIn;
    for (const uint32_t format : {552u, 553u, 554u, 555u}) {
        EXPECT_EQ(gm20b::kernelTextureFormat(codes, format, false), gm20b::kXrStandInFormat) << format;
        EXPECT_EQ(gm20b::kernelTextureFormat(codes, format, true), format) << format;
    }
    EXPECT_EQ(gm20b::kernelTextureFormat(codes, 80, false), 80u) << "a stand-in of another kind (an sRGB pair's)";
    codes.standIn = nullptr;
    EXPECT_EQ(gm20b::kernelTextureFormat(codes, 554, false), 554u);
}

// An RGB10A8_2P_XR10 texel buffer over a surface's plane, held as BGR10_XR's words: an M4 Max's kernel stores
// 0x27f80100 for (0.5, 0.25, -0.25, 0.75) into each word, bits 30 and 31 clear, and nothing else, and reads alpha 0. A
// kernel that may store it is compiled for its format, whose texel buffer the translator holds as the words alone; one
// over a buffer keeps its alpha bytes past the words, read through its stand-in
TEST(KernelTexture, ATwoPlaneTexelBufferOverASurfaceIsCompiledForItsWordsWhereStored)
{
    const Texture standIn;
    Texture words;
    words.type = gm20b::kTexelBufferType;
    words.standIn = &standIn;
    for (const uint32_t format : {550u, 551u}) {
        words.format = format + 4;
        EXPECT_EQ(gm20b::kernelTextureFormat(words, format, true), format) << format;
        EXPECT_EQ(gm20b::kernelTextureFormat(words, format, false), gm20b::kXrStandInFormat) << format;
        Texture buffer = words;
        buffer.format = format;
        EXPECT_EQ(gm20b::kernelTextureFormat(buffer, format, true), gm20b::kXrStandInFormat) << format;
    }
}

// RGB10A8_2P_XR10 read alone over a surface's last plane is held as BGR10_XR's words under BGR10_XR's header
// (TegraGM20BMetalXrPlanes.h xrWordsTexture), which an M4 Max reads with alpha 0. Boot 2's kernels were compiled for
// 550, whose view is a word and an alpha byte side by side, which no header of the texture is: admission failed them
// (gpu.gm20b.xr_next_plane, xr_planes_off16). They read the words in BGR10_XR, a channel of alpha's zero
TEST(KernelTexture, WordsAloneAreReadInTheirOwnFormatWithAlphaZero)
{
    const Texture standIn;
    Texture words;
    words.standIn = &standIn;
    for (const uint32_t format : {550u, 551u}) {
        words.format = format + 4;
        for (const bool codes : {false, true})
            EXPECT_EQ(gm20b::kernelTextureFormat(words, format, codes), format + 4) << format;
        uint8_t own[4] = {2, 3, 4, 5}, swizzled[4] = {5, 1, 4, 0};
        gm20b::kernelTextureSwizzle(words, format, own);
        gm20b::kernelTextureSwizzle(words, format, swizzled);
        EXPECT_EQ(std::vector<uint8_t>(own, own + 4), (std::vector<uint8_t>{2, 3, 4, 0})) << format;
        EXPECT_EQ(std::vector<uint8_t>(swizzled, swizzled + 4), (std::vector<uint8_t>{0, 1, 4, 0})) << format;
    }
    Texture planes = words;
    planes.format = 550;
    uint8_t own[4] = {2, 3, 4, 5};
    gm20b::kernelTextureSwizzle(planes, 550, own);
    EXPECT_EQ(own[3], 5) << "a 550 held as 550 keeps its alpha";
    Texture buffer = words;
    buffer.format = 554;
    buffer.type = gm20b::kTexelBufferType;
    EXPECT_EQ(gm20b::kernelTextureFormat(buffer, 550, false), gm20b::kXrStandInFormat) << "a texel buffer's own rule";
}

// A state's reflection names the texture slots its kernel may store: a read-write or write-only texture binding's
// slots (an array's each), not a read-only one's nor a buffer's
TEST(KernelTexture, TheSlotsAKernelStoresAreItsWritableTextureBindings)
{
    struct Node {
        uint64_t values[8];
    };

    struct Range {
        uint32_t first, count;
    };

    struct View {
        const Range *stages;
        const Node *nodes;
        const uint32_t *edges;
    };

    const Node nodes[] = {{{2, 0, 0, 0, 0, 0, 0, 0}},   // texture 0, read-only
                          {{2, 1, 3, 0, 0, 0, 0, 0}},   // texture 3, read-write
                          {{2, 2, 5, 0, 0, 0, 0, 2}},   // textures 5 and 6, write-only
                          {{0, 1, 1, 0, 0, 0, 0, 0}}};  // buffer 1, read-write
    const uint32_t edges[] = {0, 1, 2, 3};
    const Range kernel = {0, 4};
    EXPECT_EQ(gm20b::kernelTexturesWritten(View{&kernel, nodes, edges}), 1u << 3 | 1u << 5 | 1u << 6);
}

// The slots a kernel reads as floats are its texture bindings of float components, not half ones nor a buffer's
TEST(KernelTexture, TheSlotsAKernelReadsAsFloatsAreItsFloatTextureBindings)
{
    struct Node {
        uint64_t values[8];
    };

    struct Range {
        uint32_t first, count;
    };

    struct View {
        const Range *stages;
        const Node *nodes;
        const uint32_t *edges;
    };

    const Node nodes[] = {{{2, 0, 0, 0, 0, 0, 3, 1}},    // texture 0, float
                          {{2, 0, 1, 0, 0, 0, 16, 1}},   // texture 1, half
                          {{2, 1, 4, 0, 0, 0, 3, 2}},    // textures 4 and 5, float
                          {{0, 0, 2, 3, 0, 0, 16, 0}}};  // buffer 2 of floats
    const uint32_t edges[] = {0, 1, 2, 3};
    const Range kernel = {0, 4};
    EXPECT_EQ(gm20b::kernelTexturesFloat(View{&kernel, nodes, edges}), 1u << 0 | 1u << 4 | 1u << 5);
    EXPECT_EQ(gm20b::kernelTexturesWritten(View{&kernel, nodes, edges}), 1u << 4 | 1u << 5);
}

// gpu.compute.exact_round (boot 2): 6,628 of BGR10_XR's and BGRA10_XR's float channels read through the RGBA16Float
// stand-in were a half's rounding off, every half read right. A slot read as floats takes the codes beside samplers
// the translator's fetches all take; a slot a kernel may store of a texture made with ShaderWrite takes them as before
TEST(KernelTexture, AFloatReadTakesTheCodesBesideSamplersTheFetchesTake)
{
    const uint32_t floats = 1u << 2, written = 1u << 5;
    EXPECT_TRUE(gm20b::kernelReadsCodes(2, false, written, floats, true));
    EXPECT_FALSE(gm20b::kernelReadsCodes(2, false, written, floats, false)) << "beside a mip filter";
    EXPECT_FALSE(gm20b::kernelReadsCodes(3, false, written, floats, true)) << "a half read";
    EXPECT_TRUE(gm20b::kernelReadsCodes(5, true, written, floats, false));
    EXPECT_FALSE(gm20b::kernelReadsCodes(5, false, written, floats, true)) << "made without ShaderWrite";
}

// m2v's extended_sampler takes a cubic filter, or one filter for minification and magnification with no mip filter and
// no anisotropy (MTLSamplerMinMagFilter: nearest 0, linear 1, cubic 2)
TEST(KernelTexture, TheCodesFetchesTakeOneFilterAndNoMipFilterOrAnisotropy)
{
    EXPECT_TRUE(gm20b::xrCodesSampler(0, 0, 0, 1));
    EXPECT_TRUE(gm20b::xrCodesSampler(1, 1, 0, 1));
    EXPECT_TRUE(gm20b::xrCodesSampler(2, 1, 0, 1));
    EXPECT_FALSE(gm20b::xrCodesSampler(1, 0, 0, 1));
    EXPECT_FALSE(gm20b::xrCodesSampler(0, 0, 1, 1));
    EXPECT_FALSE(gm20b::xrCodesSampler(1, 1, 2, 1));
    EXPECT_FALSE(gm20b::xrCodesSampler(1, 1, 0, 4));
}

// A kernel reading an extended-range texture's codes over rows off 32 bytes reaches them through its pool image, which
// the encoder fills; one reading its stand-in reads the stand-in
TEST(KernelTexture, ACodesReadOfAPooledTextureGoesThroughItsShadow)
{
    const Texture standIn, image;
    Texture t;
    t.standIn = &standIn;
    t.sampled = &image;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::StandIn);
    EXPECT_EQ(gm20b::kernelTextureForm(t, false), KernelTextureForm::Shadow);
    t.sampled = nullptr;
    EXPECT_EQ(gm20b::kernelTextureForm(t, false), KernelTextureForm::InPlace);
}

TEST(KernelTexture, AMipmappedTextureIsSampledInPlace)
{
    Texture t;
    const int chain = 0;
    t.chain = &chain;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::InPlace);
}

TEST(KernelTexture, RowsTheHeaderCannotAddressGoThroughTheShadow)
{
    const Texture image;
    Texture t;
    t.sampled = &image;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::Shadow);
    t.surfaceView = true;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::Shadow);
}

// A texel buffer over a surface the kernel laid out block-linear (texel_rows.h) is reached through its pool, filled
// from the surface's rows and copied back around its encoder; an extended-range one too, its stand-in drawn from that pool
TEST(KernelTexture, ATexelBufferOverABlockLinearSurfaceGoesThroughItsPool)
{
    const int rows = 0;
    const Texture standIn;
    Texture t;
    t.type = gm20b::kTexelBufferType;
    t.texelRows = &rows;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::Shadow);
    t.standIn = &standIn;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::Shadow);
}

TEST(KernelTexture, EveryOtherTermHasItsForm)
{
    const Texture other;
    Texture t;
    t.planar = true;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::Converted);
    t = Texture();
    t.ycbcr = true;
    t.sampled = &other;  // a plane's pool image is the conversion's, not the kernel's
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::Converted);
    t = Texture();
    t.standIn = &other;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::StandIn);
    t = Texture();
    t.tic = 0;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::NoHeader);
    t = Texture();
    t.memoryless = true;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::NoHeader);
    t = Texture();
    t.xrPlanes = true;
    t.standIn = &other;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::TwoPlanes);
}

// A memoryless RGB10A8_2P_XR10 texture's planes have no memory a kernel could reach (xr_texture.cpp
// createXrMemoryless): a load reads header 0, a store fails its command buffer, as any memoryless texture's
TEST(KernelTexture, AMemorylessTwoPlaneTextureHasNoHeader)
{
    Texture t;
    t.xrPlanes = true;
    t.memoryless = true;
    EXPECT_EQ(gm20b::kernelTextureForm(t), KernelTextureForm::NoHeader);
}

}  // namespace
