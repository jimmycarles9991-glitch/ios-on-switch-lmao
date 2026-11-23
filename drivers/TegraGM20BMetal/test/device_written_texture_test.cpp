// SPDX-License-Identifier: MIT
// A fragment program's storage write into a texture gm20b::Device samples through another image: an extended-range texture's
// RGBA16Float stand-in (xr.h), or the block-linear shadow of rows its own header cannot address (Texture::sampled). The
// draw stores through the texture's own codes header, or through the shadow's, which the frontend copies back after the
// pass (TegraGM20BMetalRangeForms.mm writtenBack); a slot the program only samples still reads the stand-in. A two-plane
// RGB10A8_2P_XR10 texture is stored through the RG32Uint image of its texels the pass holds (merged from the planes before
// it, split back after: TegraGM20BMetalXrPlanes.mm), refused without one. Read off the set-0 word the draw uploaded.
#include "device_harness.h"
#include "../record.h"
#include "../written_textures.h"
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kBGRA10XR = 552, kRGB10A8XR = 550, kRGBA16Float = 115, kRG32Uint = 103, kTriangle = 3,
                   kSlot = 1;

class DeviceWrittenTexture : public harness::DeviceTest {
protected:
    gm20b::Texture target;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, target));
    }

    // A record whose fragment program reads the texture at fragment slot kSlot, storing into it as `written` (0: samples
    // it alone)
    gm20b::Pipeline *record(uint64_t hash, uint32_t written)
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        gm20b::PackPipeline &pp = r.pipeline;
        pp.keyHash = hash;
        pp.flags = gm20b::PipelinePushMetadata;
        pp.setBytes = 16;
        gm20b::setEntryCount(pp, 1);
        pp.entry[0] = {0, gm20b::EntryTexture, 2, kSlot, 0, 0, 0};
        pp.writtenFormat[kSlot] = uint16_t(written);
        r.blob = {0x1a00, 0x1111};
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // One draw of `pl` with `t` at kSlot: "" drawn, else why not; `header` the texture header the draw's set 0 names there
    std::string draw(gm20b::Pipeline *pl, const gm20b::Texture &t, uint32_t &header, gm20b::Refusal *refusal = nullptr,
                     const gm20b::Texture *image = nullptr)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.writtenPlanes[0] = {&t, image};
        gm20b::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(pl);
        e->setTexture(2, kSlot, &t);
        const char *why = nullptr;
        if (!e->draw(kTriangle, 0, 3, 1, 0, &why)) {
            if (refusal)
                *refusal = cb->refusal;
            cb->abandon();
            return why ? why : "refused";
        }
        const uint64_t set = gm20b::DeviceHarness::preparedSet(*e);
        const uint8_t *words = gm20b::DeviceHarness::upload(dev, set, 4);
        header = words ? (words[0] | words[1] << 8 | (words[2] & 0xf) << 16) : 0;
        e->end();
        uint32_t seqno = 0;
        if (!cb->commit(seqno)) {
            cb->abandon();
            return "not committed";
        }
        return "";
    }
};

TEST_F(DeviceWrittenTexture, AnExtendedRangeTextureIsStoredThroughItsOwnCodesNotItsStandIn)
{
    gm20b::Texture xr, standIn;
    ASSERT_TRUE(dev.createTexture(32, 32, kBGRA10XR, xr));
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA16Float, standIn));
    xr.standIn = &standIn;
    gm20b::Pipeline *written = record(0x77e1, kBGRA10XR), *sampled = record(0x77e2, 0);
    ASSERT_NE(written, nullptr);
    ASSERT_NE(sampled, nullptr);
    uint32_t header = 0;
    ASSERT_EQ(draw(written, xr, header), "");
    EXPECT_EQ(header, xr.tic) << "the codes, as the program stores them";
    ASSERT_EQ(draw(sampled, xr, header), "");
    EXPECT_EQ(header, standIn.tic) << "a sample reads the stand-in";
    xr.standIn = nullptr;
}

TEST_F(DeviceWrittenTexture, RowsItsHeaderCannotAddressAreStoredThroughTheShadow)
{
    gm20b::Texture texture, shadow;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, texture));
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, shadow));
    texture.sampled = &shadow;
    texture.tic = shadow.tic;
    gm20b::Pipeline *written = record(0x77e3, kRGBA8);
    ASSERT_NE(written, nullptr);
    uint32_t header = 0;
    ASSERT_EQ(draw(written, texture, header), "");
    EXPECT_EQ(header, shadow.tic);
    texture.sampled = nullptr;
}

TEST_F(DeviceWrittenTexture, ATwoPlaneTextureIsStoredThroughThePassImageOfItsTexels)
{
    gm20b::Texture xr, image;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGB10A8XR, xr));
    ASSERT_TRUE(dev.createTexture(32, 32, kRG32Uint, image));
    xr.xrPlanes = true;
    gm20b::Pipeline *written = record(0x77e4, kRGB10A8XR);
    ASSERT_NE(written, nullptr);
    uint32_t header = 0;
    ASSERT_EQ(draw(written, xr, header, nullptr, &image), "");
    EXPECT_EQ(header, image.tic) << "the RG32Uint image merged from the planes, split back after the pass";
    gm20b::Refusal refusal;
    EXPECT_NE(draw(written, xr, header, &refusal), "") << "no image: nothing holds its texels";
    EXPECT_EQ(refusal.id, gm20b::RefWrittenPlanes);
    xr.xrPlanes = false;
}

// An RGB10A8_2P_XR10 texel buffer over a surface's plane is held as BGR10_XR's words (TegraGM20BMetalSurfaceTextures.mm
// makeSurfaceTexelBuffer): a record compiled for RGB10A8_2P_XR10 stores through that header, as an M4 Max's fragment
// function stores the words alone, while a sample reads the stand-in. A 2D BGR10_XR texture is no such buffer
TEST_F(DeviceWrittenTexture, ATwoPlaneTexelBufferIsStoredThroughItsWordsHeader)
{
    constexpr uint32_t kBGR10XR = 554;
    gm20b::Buffer b;
    ASSERT_TRUE(dev.createBuffer(1 << 16, b));
    gm20b::Texture words, standIn, plain;
    ASSERT_TRUE(dev.createTexelBuffer(b, 0, 64, kBGR10XR, words));
    ASSERT_TRUE(dev.createTexture(64, 1, kRGBA16Float, standIn));
    ASSERT_TRUE(dev.createTexture(32, 32, kBGR10XR, plain));
    words.standIn = &standIn;
    gm20b::Pipeline *written = record(0x77e7, kRGB10A8XR), *sampled = record(0x77e8, 0);
    ASSERT_NE(written, nullptr);
    ASSERT_NE(sampled, nullptr);
    uint32_t header = 0;
    ASSERT_EQ(draw(written, words, header), "");
    EXPECT_EQ(header, words.tic) << "the words, which the program stores alone";
    ASSERT_EQ(draw(sampled, words, header), "");
    EXPECT_EQ(header, standIn.tic);
    gm20b::Refusal refusal;
    EXPECT_EQ(draw(written, plain, header, &refusal), "a written texture of another format than its program stores");
    EXPECT_EQ(refusal.id, gm20b::RefWrittenTexture);
    words.standIn = nullptr;
    dev.destroyTexture(words);
    dev.destroyBuffer(b);
}

// A draw takes the variant made for the formats its written textures have (TegraGM20BMetalCubic.mm reboundTextures): a record
// made for the bound format stores through it, the state's own of another format is refused as its own kind (the
// driver's error, TegraGM20BMetalDrawErrors.mm)
TEST_F(DeviceWrittenTexture, AWrittenTextureTakesTheVariantOfItsOwnFormat)
{
    constexpr uint32_t kR32Uint = 53;
    gm20b::Texture texture;
    ASSERT_TRUE(dev.createTexture(32, 32, kR32Uint, texture));
    gm20b::Pipeline *own = record(0x77e5, kRGBA8);
    ASSERT_NE(own, nullptr);
    const std::vector<gm20b::WrittenTexture> rebound = gm20b::reboundWritten(*own->rec, [&](uint32_t slot) {
        return slot == kSlot ? texture.format : 0u;
    });
    ASSERT_EQ(rebound.size(), 1u);
    gm20b::Pipeline *variant = record(0x77e6, rebound[0].format);
    ASSERT_NE(variant, nullptr);
    ASSERT_TRUE(gm20b::compiledFor(*variant->rec, rebound));
    uint32_t header = 0;
    EXPECT_EQ(draw(variant, texture, header), "");
    EXPECT_EQ(header, texture.tic);
    gm20b::Refusal refusal;
    EXPECT_EQ(draw(own, texture, header, &refusal), "a written texture of another format than its program stores");
    EXPECT_EQ(refusal.id, gm20b::RefWrittenTexture);
    EXPECT_EQ(refusal.b, kR32Uint);
    EXPECT_EQ(refusal.c, kRGBA8);
}

}  // namespace
