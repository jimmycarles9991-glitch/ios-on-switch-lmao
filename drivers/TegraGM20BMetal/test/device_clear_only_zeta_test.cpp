// SPDX-License-Identifier: MIT
// A clear-only pass's Clear of a stored depth larger than its render area reaches the whole texture, as its colour's does
// (pass_end.cpp ownZetaClear), on gm20b::Device: within the texture's own layers, under a four-sample zeta's own sample
// layout, over a turned pass's whole plane, and leaving the zcull region the pass sets initialised by the in-area clear.
#include "device_forms.h"
#include "TegraGM20BZcullABI.h"
#include "../mme.h"
#include "../zeta_colour.h"

namespace {

constexpr uint32_t kStencil8 = 253;

// A zeta clear's target, rectangle and the layers it clears
struct ZetaClear {
    uint64_t zeta = 0;
    uint32_t sizeC = 0, horizontal = 0, vertical = 0, payload = 0, layers = 1, antiAlias = ~0u;
};

std::vector<ZetaClear> zetaClears(const std::vector<Method> &ms)
{
    std::vector<ZetaClear> out;
    ZetaClear now;
    uint32_t upper = 0;
    bool layered = false;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_ZT_A)
            upper = x.value;
        else if (x.method == NVB197_SET_ZT_A + 4)
            now.zeta = uint64_t(upper) << 32 | x.value;
        else if (x.method == NVB197_SET_ZT_SIZE_C)
            now.sizeC = x.value;
        else if (x.method == NVB197_SET_CLEAR_RECT_HORIZONTAL)
            now.horizontal = x.value;
        else if (x.method == NVB197_SET_CLEAR_RECT_VERTICAL)
            now.vertical = x.value;
        else if (x.method == NVB197_SET_ANTI_ALIAS)
            now.antiAlias = x.value;
        else if (x.method == NVB197_CLEAR_SURFACE && (x.value & 3)) {
            now.payload = x.value;
            now.layers = 1;
            out.push_back(now);
        } else if (x.method == NVB197_CALL_MME_MACRO(gm20b::mme::kClearLayers)) {
            now.payload = x.value;
            layered = (x.value & 3) != 0;
        } else if (x.method == NVB197_CALL_MME_MACRO(gm20b::mme::kClearLayers) + 4 && layered) {
            now.layers = x.value;
            out.push_back(now);
            layered = false;
        }
    }
    return out;
}

constexpr uint32_t rect(uint32_t from, uint32_t to)
{
    return to << 16 | from;
}

// A layered clear-only pass of 4 layers over a separate Depth32Float and Stencil8 of 2 layers each, larger than its area:
// each texture cleared whole over its own 2 layers, none past them
TEST_F(DevicePassForms, AClearOnlyPassClearsASeparatePairWithinItsOwnLayers)
{
    gm20b::Texture colour, depth, stencil;
    ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, gm20b::fx::kTexTwoDArray, colour));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, depth, 2));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kStencil8, 1, false, stencil, 2));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.layers = 4;
    p.depthStencil.texture = &depth;
    p.depthStencil.clearDepth = true;
    p.depthStencil.clearStencil = true;
    p.separate = {&depth, &stencil, false, false, true, true};
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    size_t own = 0;
    for (const ZetaClear &c : zetaClears(ms)) {
        if (c.zeta != depth.va && c.zeta != stencil.va)
            continue;
        own++;
        EXPECT_EQ(c.horizontal, rect(0, 64));
        EXPECT_EQ(c.vertical, rect(0, 64));
        EXPECT_EQ(c.sizeC, 2u);
        EXPECT_EQ(c.layers, 2u);
    }
    EXPECT_EQ(own, 2u) << "the depth's and the stencil's";
}

// A clear-only mixed pass (a one-sample colour beside a larger four-sample depth): the depth cleared whole under its own
// 2X2 sample layout, the colour then cleared under its 1X1, the mode off for both, and no clear of the depth's area after
TEST_F(DevicePassForms, AClearOnlyMixedPassClearsItsDepthInTheDepthsSampleLayout)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 4, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    const auto clears = zetaClears(ms);
    ASSERT_EQ(clears.size(), 1u);
    EXPECT_EQ(clears[0].zeta, zeta.va);
    EXPECT_EQ(clears[0].horizontal, rect(0, 64));
    EXPECT_EQ(clears[0].vertical, rect(0, 64));
    EXPECT_EQ(clears[0].antiAlias, uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_2X2));
    uint32_t antiAlias = ~0u;
    bool on = false, colourCleared = false;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_ANTI_ALIAS)
            antiAlias = x.value;
        else if (x.method == NVB197_SET_TIR)
            on = x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M;
        else if (x.method == NVB197_CLEAR_SURFACE) {
            EXPECT_FALSE(on);
            if (x.value & 0x3c) {
                colourCleared = true;
                EXPECT_EQ(antiAlias, uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_1X1));
            }
        }
    }
    EXPECT_TRUE(colourCleared);
}

// A turned clear-only pass (a 48x32 target turned a quarter and not cleared, a 32x48 frame, a 16x16 render area) over a
// stored depth of its frame: the depth's plane stand-in (48x32) cleared whole from its origin
TEST_F(DevicePassForms, AClearOnlyTurnedPassClearsItsDepthsWholePlane)
{
    ownState(gm20b::turnDepthState(gm20b::kZetaFormat), "samples=1 ds=260/260 ");
    ownState(gm20b::stencilColourState(gm20b::kZetaFormat, 1, gm20b::StencilSample::Zero), "samples=1 c0=103 ");
    gm20b::Texture target, depth;
    ASSERT_TRUE(dev.createTexture(48, 32, kRGBA8, target, false, true));
    target.rotation = 1;
    ASSERT_TRUE(dev.createOwnedZeta(32, 48, gm20b::kZetaFormat, 1, true, depth));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    gm20b::PassDesc p;
    p.color[0] = kept(target, gm20b::ColorAttachment::DontCare);
    p.count = 1;
    p.width = 16;
    p.height = 16;
    p.depthStencil = {&depth, true, true, 1.0f, 0};
    NoDraws draws;
    gm20b::TurnedPass turned;
    const char *why = "";
    gm20b::RenderEncoder *e = nullptr;
    ASSERT_TRUE(turned.begin(*cb, p, draws, &why)) << why;
    ASSERT_NE(e = cb->render(p, &why), nullptr) << why;
    ASSERT_NE(p.depthStencil.texture, &depth) << "the pass renders a stand-in of its plane";
    const uint64_t standIn = p.depthStencil.texture->va;
    e->end();
    ASSERT_TRUE(turned.end(*cb, &why)) << why;
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    size_t whole = 0;
    for (const ZetaClear &c : zetaClears(submitted(first))) {
        if (c.zeta != standIn)
            continue;
        EXPECT_EQ(c.horizontal & 0xffff, 0u);
        EXPECT_EQ(c.vertical & 0xffff, 0u);
        whole += c.horizontal == rect(0, 48) && c.vertical == rect(0, 32);
    }
    EXPECT_EQ(whole, 1u);
}

constexpr uint32_t kSetActiveZcullRegion = 0x1590;  // 0: the pass's region; 0x3f: none (zcull.h)
constexpr TegraGM20BAbiZcullInfo kX1Zcull = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};

class DeviceZcullClears : public DevicePassForms {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.otherMethod = [](uint32_t method, void *output) -> kern_return_t {
            if (method != TegraGM20BMethodZcullInfo)
                return kIOReturnUnsupported;
            *static_cast<TegraGM20BAbiZcullInfo *>(output) = kX1Zcull;
            return KERN_SUCCESS;
        };
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    }
};

// A clear-only pass taking a zcull region over a stored depth larger than its area: the whole depth cleared outside any
// region, then the area's depth cleared again once the pass's region is set, which that clear initialises
TEST_F(DeviceZcullClears, AClearOnlyPassClearsItsDepthAgainInsideItsZcullRegion)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    uint32_t region = ~0u;
    bool whole = false, inRegion = false;
    uint32_t horizontal = 0;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_3D)
            continue;
        if (x.method == kSetActiveZcullRegion)
            region = x.value;
        else if (x.method == NVB197_SET_CLEAR_RECT_HORIZONTAL)
            horizontal = x.value;
        else if (x.method == NVB197_CLEAR_SURFACE && (x.value & 1)) {
            if (horizontal == rect(0, 64)) {
                whole = true;
                EXPECT_EQ(region, 0x3fu) << "the whole depth's clear outside any region";
            } else if (region == 0) {
                inRegion = true;
                EXPECT_EQ(horizontal, rect(0, 32));
            }
        }
    }
    EXPECT_TRUE(whole);
    EXPECT_TRUE(inRegion);
}

}  // namespace
