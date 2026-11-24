// SPDX-License-Identifier: MIT
// Multisampled passes' forms on gm20b::Device: a one-sample colour beside a four-sample depth (target-independent
// rasterisation), a multisampled clear's reach, and which sample counts take a zcull region.
#include "device_forms.h"
#include "TegraGM20BZcullABI.h"

namespace {

// A one-sample colour beside a four-sample depth: the depth cleared under the zeta's own layout (SET_ANTI_ALIAS 2X2), then
// target-independent rasterisation on (the colour's 1X1, raster 2X2, no modulation), and off again at the pass's end
TEST_F(DevicePassForms, AOneSampleColourBesideAFourSampleDepthRasterisesAtTheDepthsSamples)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    EXPECT_EQ(addressOf(ms, NVB197_SET_ZT_A), zeta.va);
    EXPECT_EQ(addressOf(ms, NVB197_SET_COLOR_TARGET_A(0)), colour.va);
    uint32_t antiAlias = ~0u, raster = ~0u, modulation = ~0u;
    bool zetaCleared = false, on = false;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_ANTI_ALIAS)
            antiAlias = x.value;
        else if (x.method == NVB197_SET_ANTI_ALIAS_RASTER)
            raster = x.value;
        else if (x.method == NVB197_SET_TIR_MODULATION)
            modulation = x.value;
        else if (x.method == NVB197_CLEAR_SURFACE && (x.value & 1) && !on) {
            EXPECT_EQ(antiAlias, uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_2X2)) << "the zeta's own layout";
            zetaCleared = true;
        } else if (x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M && !on) {
            on = true;
            EXPECT_TRUE(zetaCleared) << "the depth is cleared before the mode is on";
            EXPECT_EQ(antiAlias, uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_1X1));
            EXPECT_EQ(raster, uint32_t(NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_2X2));
            EXPECT_EQ(modulation, uint32_t(NVB197_SET_TIR_MODULATION_COMPONENT_SELECT_NO_MODULATION));
        }
    }
    EXPECT_TRUE(on);
    EXPECT_EQ(lastOf(ms, NVB197_SET_TIR), uint32_t(NVB197_SET_TIR_MODE_DISABLED));
    EXPECT_EQ(lastOf(ms, NVB197_SET_ANTI_ALIAS_RASTER), uint32_t(NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_1X1));
    EXPECT_EQ(lastOf(ms, NVB197_SET_ANTI_ALIAS), uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_1X1));
}

// The colour of a mixed pass cleared over a loaded depth (gpu.gm20b.mixed_samples' pass) or a cleared one: its
// CLEAR_SURFACE comes with the zeta deselected and the mode off (the device: class error 0x124,
// INVALID_TIR_COMBINATION02, at that clear under the mode); the zeta then back, the mode on after every clear
TEST_F(DevicePassForms, AMixedPassClearsItsColourBeforeTheModeWithTheZetaDeselected)
{
    for (const bool depthCleared : {false, true}) {
        SCOPED_TRACE(depthCleared ? "depth cleared" : "depth loaded");
        gm20b::Texture colour, zeta;
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.count = 1;
        p.depthStencil = {&zeta, depthCleared, false, 0.25f, 0};
        std::string why;
        const auto ms = pass(p, &why);
        ASSERT_FALSE(ms.empty()) << why;
        uint32_t zetaSelected = ~0u;
        bool on = false, colourCleared = false;
        for (const Method &x : ms) {
            if (x.subc != gm20b::SUBC_3D)
                continue;
            if (x.method == NVB197_SET_ZT_SELECT) {
                zetaSelected = x.value;
            } else if (x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M) {
                on = true;
                EXPECT_EQ(zetaSelected, 1u) << "the zeta selected again for the draws";
            } else if (x.method == NVB197_CLEAR_SURFACE && (x.value & 0x3c)) {
                colourCleared = true;
                EXPECT_FALSE(on) << "a colour clear under the mode";
                EXPECT_EQ(zetaSelected, 0u) << "a colour clear beside the four-sample zeta";
                EXPECT_EQ(x.value & 3, 0u) << "the zeta cleared with the colour";
            } else if (x.method == NVB197_CLEAR_SURFACE) {
                EXPECT_FALSE(on) << "a zeta clear under the mode";
                EXPECT_EQ(zetaSelected, 1u);
            }
        }
        EXPECT_TRUE(colourCleared);
        EXPECT_TRUE(on);
    }
}

// Each state's colour rule in a mixed pass (mixed_samples.h): a state of one sample merges the pixel's raster samples, a
// state of four keeps sample 0's; the depth is written at every covered sample under both (SET_SAMPLE_MASK all)
TEST_F(DevicePassForms, AMixedPassSetsEachStatesCoverageRule)
{
    gm20b::Pipeline *one = state(0xb1, "samples=1 c0=70 ds=252/0 "), *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(four, nullptr);
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    const auto ms = pass(
        p,
        [&](gm20b::RenderEncoder &e, const char **refused) {
            e.setPipeline(four);
            if (!e.draw(kTriangleStrip, 0, 4, 1, 0, refused))
                return false;
            e.setPipeline(one);
            return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
        },
        &why);
    ASSERT_FALSE(ms.empty()) << why;
    const std::vector<uint32_t> controls = values(ms, NVB197_SET_TIR_CONTROL);
    ASSERT_GE(controls.size(), 3u);
    EXPECT_EQ(controls[controls.size() - 2], gm20b::tirControl(4));
    EXPECT_EQ(controls.back(), gm20b::tirControl(1));
    for (const uint32_t m : values(ms, NVB197_SET_SAMPLE_MASK_X0_Y0))
        EXPECT_EQ(m, 0xffffu);
}

constexpr uint32_t kSetActiveZcullRegion = 0x1590;  // 0: the pass's region; 0x3f: none (zcull.h)
// The X1's zcull layout (one GPC, 2048 aliquots), as zcull_test.cpp holds it
constexpr TegraGM20BAbiZcullInfo kX1Zcull = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};

// The device over a kernel that reports zcull
class DeviceZcullPasses : public DevicePassForms {
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

    // What one pass over a cleared depth of `zetaSamples` beside a memoryless colour of `samples` sets its active zcull
    // region to
    std::vector<uint32_t> regions(uint32_t samples, uint32_t zetaSamples)
    {
        gm20b::Texture colour, zeta;
        EXPECT_TRUE(dev.createMemoryless(32, 32, kRGBA8, colour, samples));
        EXPECT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, zetaSamples, true, zeta));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.color[0].store = false;
        p.count = 1;
        p.depthStencil = {&zeta, true, false, 1.0f, 0};
        std::string why;
        const auto ms = pass(p, &why);
        EXPECT_FALSE(ms.empty()) << why;
        return values(ms, kSetActiveZcullRegion);
    }
};

// A pass takes a region at any sample count its colour and depth share, as NVK's begin does (the render area's pixels at
// any sample count); a mixed pass, whose depth has more samples than its colour, takes none
TEST_F(DeviceZcullPasses, APassOfOneSampleCountTakesARegionAndAMixedOneNone)
{
    for (const uint32_t samples : {1u, 4u}) {
        const auto set = regions(samples, samples);
        EXPECT_NE(std::find(set.begin(), set.end(), 0u), set.end()) << samples << " samples";
    }
    const auto mixed = regions(1, 4);
    EXPECT_EQ(std::find(mixed.begin(), mixed.end(), 0u), mixed.end());
    EXPECT_NE(std::find(mixed.begin(), mixed.end(), 0x3fu), mixed.end());
}

// The pass-end orders a mixed pass's probe rows write (mixed_samples.h TirEnd, Device::probeTir; HW Test
// gpu.gm20b.mixed_end_rows): the mode's, raster count's, zeta select's and raster enable's methods after the mode's last
// enable
TEST_F(DevicePassForms, AMixedPassEndsInEachProbeRowsOrder)
{
    if (!SWITCH_METAL_DIAGNOSTICS)
        GTEST_SKIP() << "the probe rows are debug builds'";
    using Tail = std::vector<std::pair<uint32_t, uint32_t>>;
    constexpr uint32_t kTir = NVB197_SET_TIR, kRaster = NVB197_SET_ANTI_ALIAS_RASTER, kZt = NVB197_SET_ZT_SELECT,
                       kEnable = NVB197_SET_RASTER_ENABLE;
    const auto row = [](gm20b::TirEnd end, uint32_t raster = 0) {
        return 1u | uint32_t(end) << 8 | raster << 12;
    };
    const std::vector<std::pair<uint32_t, Tail>> rows = {
        {row(gm20b::TirEnd::Mode), {{kTir, 0}, {kRaster, 0}}},
        {row(gm20b::TirEnd::RasterFirst), {{kRaster, 0}, {kTir, 0}}},
        {row(gm20b::TirEnd::ZetaOff), {{kZt, 0}, {kTir, 0}, {kRaster, 0}}},
        {row(gm20b::TirEnd::ModeOnly), {{kTir, 0}}},
        {row(gm20b::TirEnd::RasterDisabled), {{kEnable, 0}, {kTir, 0}, {kRaster, 0}, {kEnable, 1}}},
        {row(gm20b::TirEnd::Value, 6), {{kTir, 0}, {kRaster, 6}, {kZt, 0}, {kRaster, 0}}},
    };
    for (const auto &[probe, want] : rows) {
        dev.probeTir.store(probe);
        gm20b::Texture colour, zeta;
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.count = 1;
        p.depthStencil = {&zeta, true, false, 1.0f, 0};
        std::string why;
        const auto ms = pass(p, &why);
        ASSERT_FALSE(ms.empty()) << why;
        Tail got;
        for (const Method &x : ms) {
            if (x.subc != gm20b::SUBC_3D)
                continue;
            if (x.method == kTir && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M)
                got.clear();
            else if (x.method == kTir || x.method == kRaster || x.method == kZt || x.method == kEnable)
                got.emplace_back(x.method, x.value);
        }
        EXPECT_EQ(got, want) << std::hex << probe;
    }
    dev.probeTir.store(0);
}

TEST_F(DevicePassForms, OtherMixedSampleCountsAreRefused)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 2, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    EXPECT_TRUE(pass(p, &why).empty());
    EXPECT_NE(why.find("attachments of different sample counts"), std::string::npos) << why;
}

// A multisampled colour cleared over a render area smaller than it, neither stored nor memory of its own: the clear reaches
// the area rounded up to whole 32-pixel tiles, which its resolve reads (an M4 Max resolves a 64x64 Clear of a 40x24 area
// over [0, 64) x [0, 32), the clear's past the area: hwtests-r4 gpu.gm20b.resolve_codes_parts)
// A mixed pass's zcull probe rows (mixed_samples.h, Device::probeTir; HW Test gpu.gm20b.zc_mixed): a row asking for
// zcull takes a region, over the zeta's 2x2 sample grid where it asks, its words before the clears or after the mode's
// enable, and a row may leave the mode disabled after it
TEST_F(DeviceZcullPasses, AMixedPassTakesTheZcullItsProbeRowAsks)
{
    if (!SWITCH_METAL_DIAGNOSTICS)
        GTEST_SKIP() << "the probe rows are debug builds'";
    constexpr uint32_t kRegionSizeA = 0x07c0;
    const auto run = [&](uint32_t probe) {
        dev.probeTir.store(probe);
        gm20b::Texture colour, zeta;
        EXPECT_TRUE(dev.createMemoryless(32, 32, kRGBA8, colour, 1));
        EXPECT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.color[0].store = false;
        p.count = 1;
        p.depthStencil = {&zeta, true, false, 1.0f, 0};
        std::string why;
        const auto ms = pass(p, &why);
        EXPECT_FALSE(ms.empty()) << why;
        dev.probeTir.store(0);
        return ms;
    };

    // where the region (SET_ACTIVE_ZCULL_REGION 0) and the mode's enable fall, the region's width, and how often the
    // mode is disabled after its enable
    struct Seen {
        long region = -1, enable = -1;
        uint32_t width = 0, disables = 0;
    };

    const auto seen = [](const std::vector<Method> &ms) {
        Seen s;
        for (size_t i = 0; i < ms.size(); i++) {
            const Method &x = ms[i];
            if (x.subc != gm20b::SUBC_3D)
                continue;
            if (x.method == kSetActiveZcullRegion && x.value == 0 && s.region < 0)
                s.region = long(i);
            else if (x.method == kRegionSizeA && !s.width)
                s.width = x.value;
            else if (x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M && s.enable < 0)
                s.enable = long(i);
            s.disables += x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_DISABLED && s.enable >= 0;
        }
        return s;
    };
    const Seen armed = seen(run(1));
    EXPECT_LT(armed.region, 0) << "an armed row without kTirProbeZcull takes none";
    EXPECT_EQ(armed.disables, 1u) << "the pass end's";
    const Seen before = seen(run(1 | gm20b::kTirProbeZcull));
    ASSERT_GE(before.region, 0);
    EXPECT_LT(before.region, before.enable);
    const Seen grid = seen(run(1 | gm20b::kTirProbeZcull | gm20b::kTirProbeSampleGrid));
    EXPECT_EQ(grid.width, 2 * before.width);
    const Seen after = seen(run(1 | gm20b::kTirProbeZcull | gm20b::kTirProbeZcullAfter));
    ASSERT_GE(after.region, 0);
    EXPECT_GT(after.region, after.enable);
    EXPECT_EQ(seen(run(1 | gm20b::kTirProbeModeOff)).disables, 2u) << "after its enable, and the pass end's";
}

TEST_F(DevicePassForms, AMultisampledClearReachesTheAreasWholeTiles)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createMemoryless(64, 64, kRGBA8, t, 4));
    gm20b::PassDesc p;
    p.color[0] = kept(t, gm20b::ColorAttachment::Clear);
    p.color[0].store = false;
    p.count = 1;
    p.width = 40;
    p.height = 24;
    p.drawn = true;
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    const auto h = values(ms, NVB197_SET_CLEAR_RECT_HORIZONTAL), v = values(ms, NVB197_SET_CLEAR_RECT_VERTICAL);
    EXPECT_NE(std::find(h.begin(), h.end(), 64u << 16), h.end());
    EXPECT_NE(std::find(v.begin(), v.end(), 32u << 16), v.end());
}

}  // namespace
