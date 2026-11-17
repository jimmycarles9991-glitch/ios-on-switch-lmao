// SPDX-License-Identifier: MIT
// A fragment program's storage write into a texture raw::Device samples through another image: an extended-range texture's
// RGBA16Float stand-in (raw_xr.h), or the block-linear shadow of rows its own header cannot address (Texture::sampled). The
// draw stores through the texture's own codes header, or through the shadow's, which the frontend copies back after the
// pass (RawMetalRangeForms.mm writtenBack); a slot the program only samples still reads the stand-in. A two-plane
// RGB10A8_2P_XR10 texture is stored through the RG32Uint image of its texels the pass holds (merged from the planes before
// it, split back after: RawMetalXrPlanes.mm), refused without one. Read off the set-0 word the draw uploaded.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_written_textures.h"
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kBGRA10XR = 552, kRGB10A8XR = 550, kRGBA16Float = 115, kRG32Uint = 103, kTriangle = 3,
                   kSlot = 1;

class DeviceWrittenTexture : public harness::DeviceTest {
protected:
    raw::Texture target;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, target));
    }

    // A record whose fragment program reads the texture at fragment slot kSlot, storing into it as `written` (0: samples
    // it alone)
    raw::Pipeline *record(uint64_t hash, uint32_t written)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        raw::PackPipeline &pp = r.pipeline;
        pp.keyHash = hash;
        pp.flags = raw::PipelinePushMetadata;
        pp.setBytes = 16;
        raw::setEntryCount(pp, 1);
        pp.entry[0] = {0, raw::EntryTexture, 2, kSlot, 0, 0, 0};
        pp.writtenFormat[kSlot] = uint16_t(written);
        r.blob = {0x1a00, 0x1111};
        std::string why;
        raw::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // One draw of `pl` with `t` at kSlot: "" drawn, else why not; `header` the texture header the draw's set 0 names there
    std::string draw(raw::Pipeline *pl, const raw::Texture &t, uint32_t &header, raw::Refusal *refusal = nullptr,
                     const raw::Texture *image = nullptr)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.writtenPlanes[0] = {&t, image};
        raw::RenderEncoder *e = cb->render(pass);
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
        const uint64_t set = raw::DeviceHarness::preparedSet(*e);
        const uint8_t *words = raw::DeviceHarness::upload(dev, set, 4);
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
    raw::Texture xr, standIn;
    ASSERT_TRUE(dev.createTexture(32, 32, kBGRA10XR, xr));
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA16Float, standIn));
    xr.standIn = &standIn;
    raw::Pipeline *written = record(0x77e1, kBGRA10XR), *sampled = record(0x77e2, 0);
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
    raw::Texture texture, shadow;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, texture));
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, shadow));
    texture.sampled = &shadow;
    texture.tic = shadow.tic;
    raw::Pipeline *written = record(0x77e3, kRGBA8);
    ASSERT_NE(written, nullptr);
    uint32_t header = 0;
    ASSERT_EQ(draw(written, texture, header), "");
    EXPECT_EQ(header, shadow.tic);
    texture.sampled = nullptr;
}

TEST_F(DeviceWrittenTexture, ATwoPlaneTextureIsStoredThroughThePassImageOfItsTexels)
{
    raw::Texture xr, image;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGB10A8XR, xr));
    ASSERT_TRUE(dev.createTexture(32, 32, kRG32Uint, image));
    xr.xrPlanes = true;
    raw::Pipeline *written = record(0x77e4, kRGB10A8XR);
    ASSERT_NE(written, nullptr);
    uint32_t header = 0;
    ASSERT_EQ(draw(written, xr, header, nullptr, &image), "");
    EXPECT_EQ(header, image.tic) << "the RG32Uint image merged from the planes, split back after the pass";
    raw::Refusal refusal;
    EXPECT_NE(draw(written, xr, header, &refusal), "") << "no image: nothing holds its texels";
    EXPECT_EQ(refusal.id, raw::RefWrittenPlanes);
    xr.xrPlanes = false;
}

// A draw takes the variant made for the formats its written textures have (RawMetalCubic.mm reboundTextures): a record
// made for the bound format stores through it, the state's own of another format is refused as its own kind (the
// driver's error, RawMetalDrawErrors.mm)
TEST_F(DeviceWrittenTexture, AWrittenTextureTakesTheVariantOfItsOwnFormat)
{
    constexpr uint32_t kR32Uint = 53;
    raw::Texture texture;
    ASSERT_TRUE(dev.createTexture(32, 32, kR32Uint, texture));
    raw::Pipeline *own = record(0x77e5, kRGBA8);
    ASSERT_NE(own, nullptr);
    const std::vector<raw::WrittenTexture> rebound = raw::reboundWritten(*own->rec, [&](uint32_t slot) {
        return slot == kSlot ? texture.format : 0u;
    });
    ASSERT_EQ(rebound.size(), 1u);
    raw::Pipeline *variant = record(0x77e6, rebound[0].format);
    ASSERT_NE(variant, nullptr);
    ASSERT_TRUE(raw::compiledFor(*variant->rec, rebound));
    uint32_t header = 0;
    EXPECT_EQ(draw(variant, texture, header), "");
    EXPECT_EQ(header, texture.tic);
    raw::Refusal refusal;
    EXPECT_EQ(draw(own, texture, header, &refusal), "a written texture of another format than its program stores");
    EXPECT_EQ(refusal.id, raw::RefWrittenTexture);
    EXPECT_EQ(refusal.b, kR32Uint);
    EXPECT_EQ(refusal.c, kRGBA8);
}

}  // namespace
