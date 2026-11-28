// SPDX-License-Identifier: MIT
// What gm20b::Device names a draw it refuses at encode (draw_refusal.h), each id one ending of TegraGM20BMetalDrawErrors.mm
// drawFailed: a storage form in a pass of another sample count (the client's) or of another texel size (the driver's),
// a sampler header the range did not make, and a shader depth/stencil test
// of an aspect or sample count the pass lacks (the client's) or of a Depth16Unorm its pass left unconverted (the
// driver's). A planar texture sampled by comparison draws through its sampler's compare header under the texture's map.
#include "device_harness.h"
#include "../depth_stencil_state.h"
#include "../fragment_tests.h"
#include "../record.h"
#include "../shader_tests.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BTexture.h"
#include <functional>

namespace {

using Header = std::array<uint32_t, 8>;

constexpr uint32_t kRGBA8 = 70, kRGBA16Float = 115, kDepth16 = 250, kDepth32Float = 252, kTriangle = 3;
constexpr uint32_t kR8Uint = 13, kR32Float = 100;  // VkFormat
constexpr uint32_t kR32UintWord0 = 0x6014920f;     // a storage view of 4-byte texels (device_layered_fetch_test)
constexpr uint32_t kShadowWord = 5;

uint32_t storageView(uint32_t vkFormat)
{
    const uint32_t identity[4] = {};
    uint32_t word0 = 0;
    return switch_gm20b_view_word0(vkFormat, identity, true, &word0) ? word0 : 0;
}

gm20b::RuntimeRecord recordOf(uint64_t key, const char *fields, bool cbufs = false)
{
    gm20b::RuntimeRecord r;
    r.keyString = fields;
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        if (cbufs)
            p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    r.pipeline.keyHash = key;
    return r;
}

// A state fetching colour 0 through its storage form, a view of 4-byte texels
gm20b::RuntimeRecord storageForm(uint64_t key, const char *fields)
{
    gm20b::RuntimeRecord r = recordOf(key, fields);
    gm20b::PackPipeline &pp = r.pipeline;
    gm20b::setEntryCount(pp, 1);
    pp.entry[0] = {0, gm20b::EntryFetch, 2, 0, 0, 0, 0};
    pp.flags = gm20b::PipelineFetches | gm20b::PipelineStorageMetadata;
    pp.storageMask = pp.storageWriteMask = 1;
    pp.storageSamples = 1;
    pp.storageTic[0] = kR32UintWord0;
    r.blob = {0x1224, 1, 0x1a00, 0};
    return r;
}

// The storage form of a state whose program runs the depth test on its stand-in (its draws' tests DepthWrite)
gm20b::RuntimeRecord depthTested(uint64_t key)
{
    gm20b::RuntimeRecord r = storageForm(key, "samples=1 c0=70 ds=252/0 ");
    gm20b::PackPipeline &pp = r.pipeline;
    gm20b::setEntryCount(pp, 2);
    pp.entry[0] = {0, gm20b::EntryDepthTest, 2, 0, 0, 0, 0};
    pp.entry[1] = {4, gm20b::EntryFetch, 2, 0, 0, 0, 0};
    pp.flags |= gm20b::PipelinePushMetadata;
    pp.setBytes = 16;
    pp.shaderTests = {1, 0, gm20b::kShaderTestDepth, 0, storageView(kR32Float), 0, 0};
    pp.storageTests = uint8_t(gm20b::FragmentTests::DepthWrite);
    pp.pushMask = 0xfff;
    r.blob = {0x1224, 2, 0x1a00, 0};
    return r;
}

// A shader-side stencil's record of a state with a stencil attachment (shader_test_push.h shadowStencil)
gm20b::RuntimeRecord shadowStencil(uint64_t key)
{
    gm20b::RuntimeRecord r = recordOf(key, "samples=1 c0=70 ds=260/260 ");
    gm20b::PackPipeline &pp = r.pipeline;
    pp.flags = gm20b::PipelinePushMetadata;
    pp.setBytes = 16;
    gm20b::setEntryCount(pp, 1);
    pp.entry[0] = {0, gm20b::EntryStencilTest, 2, 0, 0, 0, 0};
    pp.shaderTests = {0, 0, gm20b::kShaderTestStencil, uint8_t(kShadowWord), 0, storageView(kR8Uint), 0};
    pp.pushMask = 0x30;
    r.blob = {0x1224, 2, 0x1a00, 0x1111};
    return r;
}

// A state sampling fragment texture 0 through fragment sampler 0, by comparison where `compares`
gm20b::RuntimeRecord sampling(uint64_t key, bool compares)
{
    gm20b::RuntimeRecord r = recordOf(key, "samples=1 c0=70 ds=0/0 sampler", true);
    gm20b::PackPipeline &pp = r.pipeline;
    gm20b::setEntryCount(pp, 1);
    pp.entry[0] = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, uint8_t(compares ? gm20b::kEntryCompares : 0)};
    pp.setBytes = 32;
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceDrawEnds : public harness::DeviceTest {
protected:
    gm20b::Texture target, image;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target, false, true));
        ASSERT_TRUE(dev.createTexture(16, 16, kRGBA8, image));
    }

    gm20b::Pipeline *made(const gm20b::RuntimeRecord &r)
    {
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    struct Drawn {
        bool drawn = false;
        std::string why;
        gm20b::Refusal refusal;
        uint32_t handle = 0;  // set 0's first word
    };

    Drawn draw(gm20b::Pipeline *pl, const gm20b::PassDesc &pass,
               const std::function<void(gm20b::RenderEncoder &)> &bind = {})
    {
        Drawn d;
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        if (!e) {
            if (cb)
                cb->abandon();
            d.why = "no pass";
            return d;
        }
        e->setPipeline(pl);
        if (bind)
            bind(*e);
        const char *why = nullptr;
        d.drawn = e->draw(kTriangle, 0, 3, 1, 0, &why);
        if (!d.drawn) {
            d.why = why ? why : "refused";
            d.refusal = cb->refusal;
            cb->abandon();
            return d;
        }
        if (const uint8_t *set = gm20b::DeviceHarness::upload(dev, gm20b::DeviceHarness::preparedSet(*e), 4))
            memcpy(&d.handle, set, 4);
        e->end();
        uint32_t seqno = 0;
        if (!cb->commit(seqno))
            cb->abandon();
        return d;
    }

    static gm20b::PassDesc onto(gm20b::Texture &colour, gm20b::Texture *zeta = nullptr, uint8_t storage = 0)
    {
        gm20b::PassDesc p;
        p.color[0].texture = &colour;
        p.count = 1;
        p.storageMask = storage;
        p.depthStencil.texture = zeta;
        return p;
    }
};

TEST_F(DeviceDrawEnds, AStorageFormInAPassOfAnotherSampleCountIsTheClientsAndOfAnotherTexelSizeTheDrivers)
{
    gm20b::Pipeline *fetching = made(storageForm(0xde01, "samples=1 c0=70 ds=0/0 "));
    gm20b::Pipeline *half = made(storageForm(0xde02, "samples=1 c0=115 ds=0/0 "));
    ASSERT_NE(fetching, nullptr);
    ASSERT_NE(half, nullptr);
    ASSERT_EQ(fetching->storageBits, 1);
    EXPECT_TRUE(draw(fetching, onto(target, nullptr, 1)).drawn) << "one sample of four-byte texels: its own form";
    gm20b::Texture four, wide;
    ASSERT_TRUE(dev.createStoredMultisample(64, 64, kRGBA8, 4, four));
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA16Float, wide, false, true));
    const Drawn samples = draw(fetching, onto(four, nullptr, 1));
    EXPECT_FALSE(samples.drawn);
    EXPECT_EQ(samples.refusal.id, gm20b::RefStorageSamples) << samples.why;
    EXPECT_EQ(samples.refusal.c, 4u);
    const Drawn texels = draw(half, onto(wide, nullptr, 1));
    EXPECT_FALSE(texels.drawn);
    EXPECT_EQ(texels.refusal.id, gm20b::RefStorageView) << texels.why;
    EXPECT_EQ(texels.refusal.b, kRGBA16Float);
}

// Repeat on both axes, which a converted 4:2:0 texture's address map (1) clamps to the edge
TEST_F(DeviceDrawEnds, APlanarTextureSampledByComparisonTakesItsSamplersCompareHeaderUnderTheMap)
{
    gm20b::SamplerDesc desc;
    desc.sAddress = desc.tAddress = 2;
    desc.compare = 1;
    gm20b::Sampler s;
    ASSERT_TRUE(dev.createSampler(desc, s));
    ASSERT_TRUE(dev.samplerVariant(s, 1));
    ASSERT_NE(s.compareMapped[1], 0u);
    EXPECT_NE(s.compareMapped[1], s.compareTsc);
    EXPECT_NE(s.compareMapped[1], s.mapped[1]);
    gm20b::Pipeline *pl = made(sampling(0xde10, true));
    ASSERT_NE(pl, nullptr);
    gm20b::Texture planar = image;
    planar.addressMap = 1;
    const Drawn d = draw(pl, onto(target), [&](gm20b::RenderEncoder &e) {
        e.setTexture(0, &planar);
        e.setSampler(0, &s);
    });
    ASSERT_TRUE(d.drawn) << d.why;
    EXPECT_EQ(d.handle >> 20, s.compareMapped[1]);
    EXPECT_EQ(d.handle & 0xFFFFF, planar.tic);
    Header want = gm20b::DeviceHarness::tscHeader(dev, s.compareTsc);
    want[0] = (want[0] & ~0x1FFu) | (gm20b::DeviceHarness::tscHeader(dev, s.mapped[1])[0] & 0x1FFu);
    EXPECT_EQ(gm20b::DeviceHarness::tscHeader(dev, s.compareMapped[1]), want) << "the compare header, the map's modes";
    const Drawn plain = draw(pl, onto(target), [&](gm20b::RenderEncoder &e) {
        e.setTexture(0, &image);
        e.setSampler(0, &s);
    });
    ASSERT_TRUE(plain.drawn) << plain.why;
    EXPECT_EQ(plain.handle >> 20, s.compareTsc) << "a plain texture: the sampler's own compare header";
}

TEST_F(DeviceDrawEnds, ASamplerHeaderTheRangeDidNotMakeIsTheDrivers)
{
    gm20b::SamplerDesc desc;
    desc.sAddress = desc.tAddress = 2;
    gm20b::Sampler s;
    ASSERT_TRUE(dev.createSampler(desc, s));
    gm20b::Texture planar = image;
    planar.addressMap = 1;
    for (const bool compares : {false, true}) {
        gm20b::Pipeline *pl = made(sampling(0xde20 + uint64_t(compares), compares));
        ASSERT_NE(pl, nullptr);
        const Drawn d = draw(pl, onto(target), [&](gm20b::RenderEncoder &e) {
            e.setTexture(0, &planar);
            e.setSampler(0, &s);
        });
        EXPECT_FALSE(d.drawn);
        EXPECT_EQ(d.why, "a planar texture's sampler without its address map");
        EXPECT_EQ(d.refusal.id, gm20b::RefSamplerHeader);
        EXPECT_EQ(d.refusal.b, 1u);
        EXPECT_EQ(d.refusal.c, compares ? 1u : 0u);
    }
}

TEST_F(DeviceDrawEnds, AShaderTestOfAnAspectOrSampleCountThePassLacksIsTheClientsAndOfAnUnconvertedDepth16TheDrivers)
{
    gm20b::Pipeline *stencil = made(shadowStencil(0xde30)), *depth = made(depthTested(0xde31));
    ASSERT_NE(stencil, nullptr);
    ASSERT_NE(depth, nullptr);
    gm20b::Texture depthOnly, depth16, fourDepth;
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, depthOnly));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth16, 1, false, depth16));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 4, true, fourDepth));
    gm20b::DepthStencilDesc less;
    less.depthCompare = 1;
    less.depthWrite = true;
    const gm20b::DepthStencilState tested(less);
    const auto withTests = [&](gm20b::RenderEncoder &e) {
        e.setDepthStencil(&tested);
    };
    const Drawn noStencil = draw(stencil, onto(target, &depthOnly), withTests);
    EXPECT_FALSE(noStencil.drawn);
    EXPECT_EQ(noStencil.refusal.id, gm20b::RefDepthStencil) << noStencil.why;
    const Drawn mixed = draw(depth, onto(target, &fourDepth, 1), withTests);
    EXPECT_FALSE(mixed.drawn);
    EXPECT_EQ(mixed.refusal.id, gm20b::RefDepthStencil) << mixed.why;
    const Drawn unconverted = draw(depth, onto(target, &depth16, 1), withTests);
    EXPECT_FALSE(unconverted.drawn);
    EXPECT_EQ(unconverted.refusal.id, gm20b::RefTestsInvariant) << unconverted.why;
    EXPECT_EQ(unconverted.refusal.a, kDepth16);
}

TEST_F(DeviceDrawEnds, ArrayViewHandleReinterpretsCubeAndCubeArrayAsTwoDArray)
{
    auto reading = [](uint64_t key, bool arrayView) {
        gm20b::RuntimeRecord r = recordOf(key, "samples=1 c0=70 ds=0/0 ", true);
        gm20b::PackPipeline &pp = r.pipeline;
        gm20b::setEntryCount(pp, 1);
        pp.entry[0] = {
            0, gm20b::EntryTexture, 2, 0, gm20b::SamplerNone, 0, uint8_t(arrayView ? gm20b::kEntryArrayView : 0)};
        pp.setBytes = 32;
        r.blob = {0x1a00, 0x1111};
        return r;
    };
    gm20b::Pipeline *asArray = made(reading(0xde40, true)), *asCube = made(reading(0xde41, false));
    ASSERT_NE(asArray, nullptr);
    ASSERT_NE(asCube, nullptr);

    gm20b::Texture array, cubeView, cubeArray;
    ASSERT_TRUE(dev.createArray(16, 16, 12, kRGBA8, gm20b::fx::kTexTwoDArray, array, 2));
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, gm20b::fx::kTexCube, 0, 2, 6, 6, cubeView));
    ASSERT_TRUE(dev.createArray(16, 16, 12, kRGBA8, gm20b::fx::kTexCubeArray, cubeArray, 2));

    const Drawn cubeAsArray = draw(asArray, onto(target), [&](gm20b::RenderEncoder &e) {
        e.setTexture(0, &cubeView);
    });
    ASSERT_TRUE(cubeAsArray.drawn) << cubeAsArray.why;
    const uint32_t cubeTic = cubeAsArray.handle & 0xfffffu;
    EXPECT_NE(cubeTic, cubeView.tic);
    const auto cubeW = gm20b::DeviceHarness::header(dev, cubeTic);
    EXPECT_EQ((cubeW[4] >> 23) & 0xfu, gm20b::fx::kTexTwoDArray);
    EXPECT_EQ((cubeW[5] >> 16) & 0x3fffu, 5u);

    const Drawn cubeArrayAsArray = draw(asArray, onto(target), [&](gm20b::RenderEncoder &e) {
        e.setTexture(0, &cubeArray);
    });
    ASSERT_TRUE(cubeArrayAsArray.drawn) << cubeArrayAsArray.why;
    const uint32_t cubeArrayTic = cubeArrayAsArray.handle & 0xfffffu;
    EXPECT_NE(cubeArrayTic, cubeArray.tic);
    const auto cubeArrayW = gm20b::DeviceHarness::header(dev, cubeArrayTic);
    EXPECT_EQ((cubeArrayW[4] >> 23) & 0xfu, gm20b::fx::kTexTwoDArray);
    EXPECT_EQ((cubeArrayW[5] >> 16) & 0x3fffu, 11u);

    const Drawn plainArray = draw(asArray, onto(target), [&](gm20b::RenderEncoder &e) {
        e.setTexture(0, &array);
    });
    ASSERT_TRUE(plainArray.drawn) << plainArray.why;
    EXPECT_EQ(plainArray.handle & 0xfffffu, array.tic);

    const Drawn sampledCube = draw(asCube, onto(target), [&](gm20b::RenderEncoder &e) {
        e.setTexture(0, &cubeView);
    });
    ASSERT_TRUE(sampledCube.drawn) << sampledCube.why;
    EXPECT_EQ(sampledCube.handle & 0xfffffu, cubeView.tic);

    dev.destroyTexture(cubeArray);
    dev.destroyTexture(cubeView);
    dev.destroyTexture(array);
}

}  // namespace
