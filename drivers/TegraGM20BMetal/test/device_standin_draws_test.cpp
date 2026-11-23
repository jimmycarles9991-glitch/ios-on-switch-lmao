// SPDX-License-Identifier: MIT
// A client's draw never samples an extended-range texture's codes as colour (bound_texture.h drawsBound): bound with no
// stand-in (made without ShaderRead, or before the pack held its conversion state) the draw is refused for its command
// buffer to fail (TegraGM20BMetalDrawErrors.mm, gm20b:draw.no_standin). The driver's own states read codes on purpose: the
// conversion that draws a stand-in binds the texture with none (TegraGM20BMetalRangeStep.mm standIns), and draws.
#include "device_harness.h"
#include "record_pack.h"
#include "../pack.h"
#include "../record.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kBGR10XR = 554, kTriangle = 3;
constexpr const char *kConversion = "raw.xr.xr_bgr10_frag";

gm20b::RuntimeRecord sampling(uint64_t key)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    r.pipeline.keyHash = key;
    gm20b::setEntryCount(r.pipeline, 1);
    r.pipeline.entry[0] = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerNone, 0, 0};
    r.pipeline.setBytes = 16;
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceStandInDraws : public testing::Test {
protected:
    gm20b::Device dev;
    gm20b::Texture target, codes, plain;

    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        const std::vector<gm20b::RuntimeRecord> own = {sampling(gm20b::Pack::fnv(kConversion, strlen(kConversion)))};
        ASSERT_TRUE(
            dev.open(harness::recordPack(own, "standin-draws-pack.bin").c_str(), 0, err, gm20b::Device::Role::Client))
            << err;
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target, false, true));
        ASSERT_TRUE(dev.createTexture(32, 32, kBGR10XR, codes));
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, plain));
        ASSERT_EQ(codes.standIn, nullptr);
    }

    struct Drawn {
        bool drawn = false;
        std::string why;
        gm20b::Refusal refusal;
    };

    Drawn draw(gm20b::Pipeline *pl, const gm20b::Texture &sampled)
    {
        Drawn d;
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        if (!e) {
            if (cb)
                cb->abandon();
            d.why = "no pass";
            return d;
        }
        e->setPipeline(pl);
        e->setTexture(0, &sampled);
        const char *why = nullptr;
        d.drawn = e->draw(kTriangle, 0, 3, 1, 0, &why);
        if (!d.drawn) {
            d.why = why ? why : "refused";
            d.refusal = cb->refusal;
            cb->abandon();
            return d;
        }
        e->end();
        uint32_t seqno = 0;
        if (!cb->commit(seqno))
            cb->abandon();
        return d;
    }
};

TEST_F(DeviceStandInDraws, AClientsDrawOfAnExtendedRangeTextureWithNoStandInIsRefused)
{
    std::string why;
    gm20b::Pipeline *client = dev.addRecord(sampling(0x5d01), false, why);
    ASSERT_NE(client, nullptr) << why;
    EXPECT_FALSE(client->own);
    const Drawn d = draw(client, codes);
    EXPECT_FALSE(d.drawn);
    EXPECT_EQ(d.refusal.id, gm20b::RefNoStandIn) << d.why;
    EXPECT_EQ(d.refusal.a, 0u) << "the slot";
    EXPECT_EQ(d.refusal.b, kBGR10XR);
    EXPECT_TRUE(draw(client, plain).drawn) << "a texture a draw reads as it lies";
}

TEST_F(DeviceStandInDraws, TheConversionReadsTheCodesItsStandInHoldsTheValuesOf)
{
    gm20b::Pipeline *conversion = dev.ownPipeline(kConversion);
    ASSERT_NE(conversion, nullptr);
    EXPECT_TRUE(conversion->own);
    const Drawn d = draw(conversion, codes);
    EXPECT_TRUE(d.drawn) << d.why;
}

// An own state the pack lacks, made at run time (Device::addRecord), is as much the driver's own
TEST_F(DeviceStandInDraws, AnOwnStateMadeAtRunTimeReadsCodesToo)
{
    constexpr const char *kMade = "raw.xr.xr_bgra10_frag";
    std::string why;
    ASSERT_NE(dev.addRecord(sampling(gm20b::Pack::fnv(kMade, strlen(kMade))), false, why), nullptr) << why;
    gm20b::Pipeline *made = dev.ownPipeline(kMade);
    ASSERT_NE(made, nullptr);
    EXPECT_TRUE(made->own);
    EXPECT_TRUE(draw(made, codes).drawn);
}

}  // namespace
