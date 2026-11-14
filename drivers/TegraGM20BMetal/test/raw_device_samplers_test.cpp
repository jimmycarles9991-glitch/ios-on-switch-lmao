// SPDX-License-Identifier: MIT
// The sampler headers raw::Device draws with: a planar texture's address map (Texture::addressMap) applied to a sampler
// state's header and to a constexpr sampler's, which the device places with the sampler.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"

namespace {

using Header = std::array<uint32_t, 8>;

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

// RenderBox's primitive_image_fragment samples its image through a linear constexpr sampler; these address it clamped to
// zero (TSC BORDER on every axis) and clamped to the edge
constexpr Header kLinearZero = {0x260DB, 0x2a2, 0x40000, 0, 0, 0, 0, 0};
constexpr Header kLinearClamp = {0x26092, 0x2a2, 0x40000, 0, 0, 0, 0, 0};

class DeviceSamplers : public harness::DeviceTest {
protected:
    raw::Texture target, image;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createTexture(16, 16, kRGBA8, image));
    }

    // A state whose fragment texture 0 is sampled through constexpr sampler `header`
    raw::Pipeline *constexprState(const Header &header, uint64_t key)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 constexpr " + std::to_string(key);
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
            p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
        }
        r.pipeline.keyHash = key;
        r.pipeline.entries = 1;
        r.pipeline.entry[0] = {0, raw::EntryTexture, 2, 0, raw::SamplerConstexpr, 0, 0};
        r.pipeline.setBytes = 32;
        r.samplers = {header};
        r.blob = {0x1a00, 0x1111};
        std::string why;
        raw::Pipeline *made = dev.addRecord(r, false, why);
        EXPECT_NE(made, nullptr) << why;
        return made;
    }

    // A state whose fragment texture 0 is sampled through fragment sampler 0
    raw::Pipeline *slotState()
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 slot sampler";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
            p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
        }
        r.pipeline.keyHash = 0x53;
        r.pipeline.entries = 1;
        r.pipeline.entry[0] = {0, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, 0};
        r.pipeline.setBytes = 32;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        raw::Pipeline *made = dev.addRecord(r, false, why);
        EXPECT_NE(made, nullptr) << why;
        return made;
    }

    // One draw of `state` sampling `t` at fragment texture 0 (through `s` at sampler 0): its set's handle word, or *why
    bool drawnHandle(raw::Pipeline *state, const raw::Texture &t, uint32_t &handle, std::string &why,
                     const raw::Sampler *s = nullptr)
    {
        raw::CommandBuffer *cb = dev.begin();
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        if (!e) {
            why = "no pass";
            return false;
        }
        e->setPipeline(state);
        e->setTexture(0, &t);
        e->setSampler(0, s);
        const char *refused = nullptr;
        const bool drawn = e->draw(kTriangle, 0, 3, 1, 0, &refused);
        const uint8_t *set = drawn ? raw::DeviceHarness::upload(dev, raw::DeviceHarness::preparedSet(*e), 4) : nullptr;
        if (set)
            memcpy(&handle, set, 4);
        e->end();
        uint32_t seqno = 0;
        if (!cb->commit(seqno))
            cb->abandon();
        why = refused ? refused : set ? "" : "no set uploaded";
        return drawn && set;
    }
};

// Boot 21 WidgetRenderer: a two-plane 4:4:4 texture (address map 2) sampled through a constexpr sampler was refused
// (enc.planarclamp a=0 b=2) and its command buffer failed. The draw samples through the constexpr header with the
// clamps to zero sent to the edge.
TEST_F(DeviceSamplers, APlanarTextureSampledThroughAConstexprSamplerTakesItsMappedHeader)
{
    raw::Pipeline *state = constexprState(kLinearZero, 0x51);
    ASSERT_NE(state, nullptr);
    raw::Texture planar = image;
    planar.addressMap = 2;
    uint32_t handle = 0;
    std::string why;
    EXPECT_FALSE(drawnHandle(state, planar, handle, why)) << "a runtime record's maps are placed before the work";
    EXPECT_EQ(why, "a planar texture's constexpr sampler without its address map");
    ASSERT_TRUE(dev.constexprMapsPlaced());
    ASSERT_TRUE(drawnHandle(state, planar, handle, why)) << why;
    const uint32_t index = state->rec->entry[0].sampler;
    const uint32_t mapped = raw::DeviceHarness::constexprTsc(dev, index, 2);
    EXPECT_NE(mapped, raw::DeviceHarness::constexprTsc(dev, index, 0));
    EXPECT_EQ(handle >> 20, mapped);
    EXPECT_EQ(handle & 0xFFFFF, planar.tic);
    EXPECT_EQ(raw::DeviceHarness::tscHeader(dev, mapped), kLinearClamp);
    ASSERT_TRUE(drawnHandle(state, image, handle, why)) << why;
    EXPECT_EQ(handle >> 20, raw::DeviceHarness::constexprTsc(dev, index, 0)) << "a plain texture: the sampler's own";
}

// Boot 21 gpu.heap.m2v_target: an opaque white border made the sampler m2v's. The sampler's own header holds the texel a
// four-channel float texture reads at it; a texture that reads another (R8Unorm: red and alpha one) samples through a
// header of that texel, made before the work that samples it, and a draw finding none is refused, never drawn with the
// wrong border.
TEST_F(DeviceSamplers, AnOpaqueBorderReadsTheTexelOfTheTextureItSamples)
{
    constexpr uint32_t kR8 = 10;
    raw::SamplerDesc d;
    d.sAddress = d.tAddress = 5;
    d.border = raw::kBorderOpaqueWhite;
    raw::Sampler s;
    ASSERT_TRUE(dev.createSampler(d, s));
    const Header own = raw::DeviceHarness::tscHeader(dev, s.tsc);
    EXPECT_EQ((Header{own[4], own[5], own[6], own[7]}), (Header{0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000}));
    raw::Pipeline *state = slotState();
    ASSERT_NE(state, nullptr);
    uint32_t handle = 0;
    std::string why;
    ASSERT_TRUE(drawnHandle(state, image, handle, why, &s)) << why;
    EXPECT_EQ(handle >> 20, s.tsc) << "RGBA8Unorm reads the sampler's own texel";
    raw::Texture red;
    ASSERT_TRUE(dev.createTexture(16, 16, kR8, red));
    EXPECT_FALSE(drawnHandle(state, red, handle, why, &s));
    EXPECT_EQ(why, "a sampler without its opaque border's header for the texture");
    ASSERT_TRUE(dev.borderVariant(s, red));
    ASSERT_TRUE(drawnHandle(state, red, handle, why, &s)) << why;
    const uint32_t tsc = handle >> 20;
    EXPECT_EQ(tsc, s.bordered[0x9]);
    Header want = own;
    want[5] = want[6] = 0;
    want[3] &= ~0x0ffff000u;
    EXPECT_EQ(raw::DeviceHarness::tscHeader(dev, tsc), want) << "(1, 0, 0, 1): no green or blue, sRGB's too";
}

// A header no map changes is its own under every map: no header is placed for it
TEST_F(DeviceSamplers, AConstexprSamplerTheMapLeavesAloneSharesItsHeader)
{
    raw::Pipeline *state = constexprState(kLinearClamp, 0x52);
    ASSERT_NE(state, nullptr);
    ASSERT_TRUE(dev.constexprMapsPlaced());
    const uint32_t index = state->rec->entry[0].sampler;
    for (uint8_t map = 1; map < 4; map++)
        EXPECT_EQ(raw::DeviceHarness::constexprTsc(dev, index, map), raw::DeviceHarness::constexprTsc(dev, index, 0));
}

// The map on a header's own fields is the map on the sampler state's address modes (Device::samplerVariant's rule
// before it moved to the header): every mode of every axis, every map
TEST(SamplerAddressMaps, TheHeaderMapIsTheDescriptorMap)
{
    for (uint8_t s = 0; s < 6; s++)
        for (uint8_t t = 0; t < 6; t++)
            for (uint8_t r = 0; r < 6; r++)
                for (uint8_t map = 1; map < 4; map++) {
                    raw::SamplerDesc d;
                    d.minFilter = d.magFilter = 1;
                    d.sAddress = s, d.tAddress = t, d.rAddress = r;
                    raw::SamplerDesc mapped = d;
                    const bool wrapped = map & 1, clamps = map & 2;
                    for (uint8_t *m : {&mapped.sAddress, &mapped.tAddress, &mapped.rAddress})
                        if ((wrapped && *m >= 1 && *m <= 3) || (clamps && *m >= 4))
                            *m = 0;
                    uint32_t want[8], got[8];
                    raw::samplerHeader(mapped, want);
                    raw::samplerHeader(d, got);
                    raw::mapSamplerAddresses(got, map);
                    EXPECT_EQ(memcmp(want, got, sizeof want), 0)
                        << int(s) << "/" << int(t) << "/" << int(r) << " map " << int(map);
                }
}

TEST_F(DeviceSamplers, ASamplerStatesVariantIsItsHeaderUnderTheMap)
{
    raw::SamplerDesc d;
    d.sAddress = d.tAddress = 4;
    raw::Sampler s;
    ASSERT_TRUE(dev.createSampler(d, s));
    ASSERT_TRUE(dev.samplerVariant(s, 2));
    ASSERT_NE(s.mapped[2], s.tsc);
    raw::SamplerDesc edge = d;
    edge.sAddress = edge.tAddress = 0;
    Header want;
    raw::samplerHeader(edge, want.data());
    EXPECT_EQ(raw::DeviceHarness::tscHeader(dev, s.mapped[2]), want);
    ASSERT_TRUE(dev.samplerVariant(s, 1));
    EXPECT_EQ(s.mapped[1], s.tsc) << "nothing wraps";
}

}  // namespace
